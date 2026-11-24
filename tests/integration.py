"""Process/TCP contract tests; no HTTP framework or external dependency."""
import argparse
import json
import shutil
from contextlib import contextmanager
from email.utils import parsedate_to_datetime
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def assert_no_sanitizer_errors(log):
    if "Sanitizer" in log or "runtime error:" in log:
        raise AssertionError(log)


@contextmanager
def running_server(root, port=None, extra_args=()):
    with server_process(root, port, extra_args) as (process, port):
        yield port
        if process.poll() is not None:
            raise AssertionError(f"Server stopped unexpectedly: {process.returncode}")


@contextmanager
def server_process(root, port=None, extra_args=()):
    """Starts the server and yields (process, port). Cleanup terminates it if still running and
    fails on sanitizer reports. Tests that stop the server themselves use this directly."""
    port = free_port() if port is None else port
    with tempfile.TemporaryFile(mode="w+b") as log:
        process = subprocess.Popen(
            [str(BINARY), "--host", "127.0.0.1", "--port", str(port), "--root", str(root),
             *extra_args],
            stdout=subprocess.DEVNULL, stderr=log,
        )
        try:
            # Ready = the startup line is in stderr. A probe connection would go through the
            # pool and take a slot of a small `--queue`. pread leaves the file offset alone;
            # the child shares it and keeps writing there.
            deadline = time.monotonic() + 10
            while b"Listening on" not in os.pread(log.fileno(), 4096, 0):
                if process.poll() is not None:
                    log.seek(0)
                    raise AssertionError(f"Server exited during startup: {log.read().decode()}")
                if time.monotonic() >= deadline:
                    raise AssertionError("Server never became ready")
                time.sleep(0.02)
            yield process, port
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            log.seek(0)
            assert_no_sanitizer_errors(log.read().decode(errors="replace"))


def read_response(client):
    data = bytearray()
    while True:
        chunk = client.recv(65536)
        if not chunk:
            break
        data.extend(chunk)
    head, separator, body = bytes(data).partition(b"\r\n\r\n")
    if not separator:
        raise AssertionError(f"No complete HTTP response: {data[:200]!r}")
    lines = head.decode("ascii").split("\r\n")
    headers = dict(line.split(": ", 1) for line in lines[1:])
    return lines[0], headers, body


def request(port, parts=(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",)):
    with socket.create_connection(("127.0.0.1", port), timeout=8) as client:
        for part in parts:
            client.sendall(part)
            if len(parts) > 1:
                time.sleep(0.01)
        return read_response(client)


OK_STATUS = "HTTP/1.1 200 OK"
OVERLOAD_STATUS = "HTTP/1.1 503 Service Unavailable"


def request_until_ok(port, seconds=5):
    """GET / until it is served. Only 503 is retried; the server frees a slot after the
    client's close, so the very next connection can still find the pool full."""
    deadline = time.monotonic() + seconds
    while True:
        status, _, _ = request(port)
        if status == OK_STATUS:
            return
        if status != OVERLOAD_STATUS or time.monotonic() >= deadline:
            raise AssertionError(f"No 200 within {seconds}s; last status: {status!r}")
        time.sleep(0.05)


class FirstResponseTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mini-webserver-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.content = "<!doctype html><h1>첫 번째 HTTP 응답 🎉</h1>\n".encode()
        (self.root / "index.html").write_bytes(self.content)

    def assert_server_header(self, headers, port):
        self.assertEqual(headers["Server"], f"server-{port}")

    def assert_html(self, result, port, expected=None):
        status, headers, body = result
        expected = self.content if expected is None else expected
        self.assertEqual(status, "HTTP/1.1 200 OK")
        self.assertEqual(headers["Content-Type"], "text/html; charset=utf-8")
        self.assertEqual(int(headers["Content-Length"]), len(expected))
        self.assertEqual(headers["Connection"], "close")
        self.assert_server_header(headers, port)
        self.assertIsNotNone(parsedate_to_datetime(headers["Date"]))
        self.assertEqual(body, expected)

    def test_get_root_and_headers(self):
        with running_server(self.root) as port:
            self.assert_html(request(port), port)

    def test_server_header_tracks_listening_port(self):
        with running_server(self.root) as first:
            with running_server(self.root) as second:
                self.assertNotEqual(first, second)
                self.assert_html(request(first), first)
                self.assert_html(request(second), second)

    def test_fragmented_request_waits_for_complete_head(self):
        with running_server(self.root) as port:
            with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
                client.sendall(b"GET / HTTP/1.1\r\nHost: local")
                client.settimeout(0.1)
                with self.assertRaises(socket.timeout):
                    client.recv(1)
                client.sendall(b"host\r\n\r")
                client.sendall(b"\n")
                client.settimeout(3)
                self.assert_html(read_response(client), port)

    def test_lf_and_http10(self):
        with running_server(self.root) as port:
            self.assert_html(request(port, (b"GET / HTTP/1.0\nHost: localhost\n\n",)), port)

    def test_missing_file_is_404(self):
        with running_server(self.root) as port:
            for line in (b"GET /missing HTTP/1.1",):
                with self.subTest(line=line):
                    status, headers, body = request(port, (line + b"\r\nHost: localhost\r\n\r\n",))
                    self.assertEqual(status, "HTTP/1.1 404 Not Found")
                    self.assertEqual(int(headers["Content-Length"]), len(body))
                    self.assert_server_header(headers, port)

    def test_large_and_empty_file(self):
        for content in (b"", bytes(range(256)) * 16384):
            with self.subTest(size=len(content)):
                (self.root / "index.html").write_bytes(content)
                with running_server(self.root) as port:
                    self.assert_html(request(port), port, expected=content)

    def test_port_collision_is_runtime_error(self):
        with socket.socket() as occupied:
            occupied.bind(("127.0.0.1", 0))
            occupied.listen()
            port = occupied.getsockname()[1]
            result = subprocess.run(
                [str(BINARY), "--port", str(port), "--root", str(self.root)],
                capture_output=True, timeout=5,
            )
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn(b"bind", result.stderr)
            assert_no_sanitizer_errors(result.stderr.decode())

    def test_invalid_options_are_argument_errors(self):
        variants = (["--port", "0"], ["--port", "65536"], ["--unknown", "x"],
                    ["--host", "localhost"], ["--root"],
                    ["--root", str(self.root / "missing")],
                    ["--root", str(self.root / "index.html")])
        for args in variants:
            with self.subTest(args=args):
                result = subprocess.run([str(BINARY), *args], capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(b"usage:", result.stderr)
                assert_no_sanitizer_errors(result.stderr.decode())

    def test_immediate_restart_same_port(self):
        port = free_port()
        for _ in range(2):
            with running_server(self.root, port):
                self.assert_html(request(port), port)

    def test_disconnect_does_not_kill_server(self):
        content = b"x" * (4 * 1024 * 1024)
        (self.root / "index.html").write_bytes(content)
        with running_server(self.root) as port:
            with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
                client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                client.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            self.assert_html(request(port), port, expected=content)

    def test_header_limit_and_recovery(self):
        with running_server(self.root) as port:
            status, headers, _ = request(port, (b"GET / HTTP/1.1\r\nHost: h\r\nX: " + b"v" * 16384,))
            self.assertEqual(status, "HTTP/1.1 431 Request Header Fields Too Large")
            self.assert_server_header(headers, port)
            self.assert_html(request(port), port)

    def test_partial_request_has_absolute_deadline(self):
        with running_server(self.root) as port:
            with socket.create_connection(("127.0.0.1", port), timeout=4) as client:
                start = time.monotonic()
                client.sendall(b"G")
                time.sleep(3)
                client.sendall(b"E")
                status, headers, _ = read_response(client)
                elapsed = time.monotonic() - start
                self.assertEqual(status, "HTTP/1.1 408 Request Timeout")
                self.assert_server_header(headers, port)
                self.assertGreaterEqual(elapsed, 4.5)
                self.assertLess(elapsed, 6.5)
            self.assert_html(request(port), port)

    def test_symlink_or_missing_index_is_forbidden(self):
        (self.root / "index.html").unlink()
        for symlink in (False, True):
            with self.subTest(symlink=symlink):
                if symlink:
                    (self.root / "index.html").symlink_to("/etc/passwd")
                with running_server(self.root) as port:
                    status, headers, _ = request(port)
                    self.assertEqual(status, "HTTP/1.1 403 Forbidden")
                    self.assert_server_header(headers, port)


class ConcurrencyTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mini-webserver-pool-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "index.html").write_bytes(b"<!doctype html><h1>pool</h1>\n")

    def test_fifty_concurrent_clients_all_get_200(self):
        clients = 50
        results = [None] * clients
        barrier = threading.Barrier(clients)

        def client(index):
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=8) as conn:
                    barrier.wait(timeout=8)
                    # A slow sender holds a worker; a serial server would need 50 * 0.2 s.
                    conn.sendall(b"GET / HTTP/1.1\r\nHost: local")
                    time.sleep(0.2)
                    conn.sendall(b"host\r\n\r\n")
                    results[index] = read_response(conn)[0]
            except Exception as error:  # reported by the assertion below
                results[index] = repr(error)

        with running_server(self.root) as port:
            threads = [threading.Thread(target=client, args=(i,)) for i in range(clients)]
            started = time.monotonic()
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=12)
            elapsed = time.monotonic() - started
        self.assertEqual(results, ["HTTP/1.1 200 OK"] * clients)
        self.assertLess(elapsed, 6)

    def fill_worker_and_queue(self, port):
        """Hold the only worker (`busy`) and the only queue slot (`queued`) with silent
        connections. Returns both sockets; the caller closes them.

        The worker takes `busy` off the queue asynchronously, and until it has, the queue is
        still full and a second connection is answered with 503 at once. That 503 is the
        signal to close it and try again. A connection that stays silent was accepted.
        """
        busy = socket.create_connection(("127.0.0.1", port), timeout=3)  # queue is empty
        deadline = time.monotonic() + 3  # the worker drops a silent `busy` after 5 s
        try:
            while True:
                queued = socket.create_connection(("127.0.0.1", port), timeout=3)
                if not select.select([queued], [], [], 0.5)[0]:
                    return busy, queued
                queued.close()
                if time.monotonic() >= deadline:
                    raise AssertionError("Worker never took the first connection off the queue")
                time.sleep(0.05)
        except BaseException:
            busy.close()
            raise

    def test_full_queue_gets_503_and_server_recovers(self):
        with running_server(self.root, extra_args=("--workers", "1", "--queue", "1")) as port:
            busy, queued = self.fill_worker_and_queue(port)
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=3) as overflow:
                    status, headers, body = read_response(overflow)
                self.assertEqual(status, OVERLOAD_STATUS)
                self.assertEqual(headers["Connection"], "close")
                self.assertEqual(headers["Server"], f"server-{port}")
                self.assertEqual(int(headers["Content-Length"]), len(body))
            finally:
                busy.close()
                queued.close()
            request_until_ok(port)

    def test_overflow_does_not_stall_accept_on_slow_client(self):
        with running_server(self.root, extra_args=("--workers", "1", "--queue", "1")) as port:
            busy, queued = self.fill_worker_and_queue(port)
            stalled = []
            try:
                # Rejected clients never read; accept must keep going quickly anyway.
                stalled = [socket.create_connection(("127.0.0.1", port), timeout=3)
                           for _ in range(20)]
                started = time.monotonic()
                with socket.create_connection(("127.0.0.1", port), timeout=3) as last:
                    status, _, _ = read_response(last)
                self.assertEqual(status, OVERLOAD_STATUS)
                self.assertLess(time.monotonic() - started, 2)
            finally:
                for conn in (*stalled, busy, queued):
                    conn.close()

    def test_shutdown_closes_the_queued_connection_and_finishes(self):
        with server_process(self.root, extra_args=("--workers", "1", "--queue", "1")) as (
                process, port):
            busy, queued = self.fill_worker_and_queue(port)
            try:
                process.send_signal(signal.SIGTERM)
                # The queued connection never reached a worker: it is closed, not served.
                queued.settimeout(3)
                self.assertEqual(queued.recv(1), b"")
            finally:
                queued.close()
                busy.close()
            self.assertEqual(wait_exit(process), 0)

    def test_pool_options_out_of_range_are_argument_errors(self):
        variants = (["--workers", "0"], ["--workers", "129"], ["--workers", "x"],
                    ["--workers", ""], ["--workers"], ["--queue", "0"], ["--queue", "4097"],
                    ["--queue", "-1"], ["--queue", "x"], ["--queue"])
        for args in variants:
            with self.subTest(args=args):
                result = subprocess.run([str(BINARY), "--root", str(self.root), *args],
                                        capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(b"usage:", result.stderr)
                assert_no_sanitizer_errors(result.stderr.decode())

    def test_usage_mentions_pool_options(self):
        result = subprocess.run([str(BINARY), "--workers", "0"], capture_output=True, timeout=5)
        self.assertIn(b"--workers", result.stderr)
        self.assertIn(b"--queue", result.stderr)

    def test_pool_options_at_limits_start(self):
        for args in (["--workers", "1", "--queue", "1"], ["--workers", "128", "--queue", "4096"]):
            with self.subTest(args=args):
                with running_server(self.root, extra_args=args) as port:
                    status, _, _ = request(port)
                    self.assertEqual(status, OK_STATUS)


class HttpParsingTests(unittest.TestCase):
    """Parser errors over a real socket (docs/spec.md "HTTP 범위와 한도")."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mini-webserver-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "index.html").write_bytes(b"<h1>ok</h1>\n")

    def assert_standard_headers(self, headers, port, body):
        self.assertEqual(headers["Server"], f"server-{port}")
        self.assertIsNotNone(parsedate_to_datetime(headers["Date"]))
        self.assertEqual(int(headers["Content-Length"]), len(body))
        self.assertIn(headers["Connection"], ("close", "keep-alive"))

    def test_error_codes_close_the_connection(self):
        host = b"Host: h\r\n"
        cases = {
            400: [b"GET / HTTP/1.1\r\n\r\n", b"GET http://h/ HTTP/1.1\r\n" + host + b"\r\n",
                  b"GET / HTTP/1.1\r\n" + host + b" folded\r\n\r\n",
                  b"GET / HTTP/1.1\r\n" + host + host + b"\r\n",
                  b"POST / HTTP/1.1\r\n" + host + b"Content-Length: 1x\r\n\r\n",
                  b"POST / HTTP/1.1\r\n" + host + b"Content-Length: 1\r\nContent-Length: 2\r\n\r\n"],
            411: [b"POST / HTTP/1.1\r\n" + host + b"\r\n"],
            414: [b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\n" + host + b"\r\n"],
            417: [b"GET / HTTP/1.1\r\n" + host + b"Expect: 200-ok\r\n\r\n"],
            431: [b"GET / HTTP/1.1\r\n" + host + b"".join(b"X%d: v\r\n" % i for i in range(70)) + b"\r\n"],
            501: [b"PUT / HTTP/1.1\r\n" + host + b"\r\n",
                  b"POST / HTTP/1.1\r\n" + host + b"Transfer-Encoding: chunked\r\n\r\n"],
            505: [b"GET / HTTP/2.0\r\n" + host + b"\r\n"],
        }
        with running_server(self.root) as port:
            for code, requests in cases.items():
                for raw in requests:
                    with self.subTest(code=code, raw=raw[:40]):
                        with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
                            client.sendall(raw)
                            status, headers, body = read_response(client)  # EOF means closed
                        self.assertTrue(status.startswith(f"HTTP/1.1 {code} "), status)
                        self.assertEqual(headers["Connection"], "close")
                        self.assertIn(f"<h1>{code} ".encode(), body)
                        self.assert_standard_headers(headers, port, body)
            self.assertEqual(request(port)[0], "HTTP/1.1 200 OK")

    def test_post_405_has_allow_and_reads_body(self):
        with running_server(self.root) as port:
            status, headers, body = request(
                port, (b"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhel", b"lo"))
            self.assertEqual(status, "HTTP/1.1 405 Method Not Allowed")
            self.assertEqual(headers["Allow"], "GET, HEAD")
            self.assert_standard_headers(headers, port, body)

    def test_head_root_matches_get_without_body(self):
        with running_server(self.root) as port:
            get = request(port)
            with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
                client.sendall(b"HEAD / HTTP/1.1\r\nHost: x\r\n\r\n")
                status, headers, body = read_response(client)
            self.assertEqual(status, "HTTP/1.1 200 OK")
            self.assertEqual(headers["Content-Length"], get[1]["Content-Length"])
            self.assertGreater(int(headers["Content-Length"]), 0)
            self.assertEqual(body, b"")

    def test_head_has_no_body(self):
        with running_server(self.root) as port:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
                client.sendall(b"HEAD /missing HTTP/1.1\r\nHost: h\r\n\r\n")
                status, headers, body = read_response(client)
            self.assertEqual(status, "HTTP/1.1 404 Not Found")
            self.assertGreater(int(headers["Content-Length"]), 0)
            self.assertEqual(body, b"")

    def test_oversized_body_gets_413_before_body_is_sent(self):
        with running_server(self.root) as port:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
                client.sendall(b"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1048577\r\n\r\n")
                status, headers, _ = read_response(client)  # we never sent the body
            self.assertTrue(status.startswith("HTTP/1.1 413 "), status)
            self.assertEqual(headers["Connection"], "close")

    def test_body_at_the_limit_is_read(self):
        with running_server(self.root) as port:
            body = b"x" * 1048576
            head = b"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1048576\r\n\r\n"
            self.assertEqual(request(port, (head, body))[0], "HTTP/1.1 405 Method Not Allowed")

    def test_expect_100_continue_comes_first(self):
        with running_server(self.root) as port:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
                client.sendall(b"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n"
                               b"Expect: 100-continue\r\n\r\n")
                self.assertEqual(client.recv(1024), b"HTTP/1.1 100 Continue\r\n\r\n")
                client.sendall(b"data")
                status, _, _ = read_response(client)
            self.assertEqual(status, "HTTP/1.1 405 Method Not Allowed")

    def test_expect_other_value_is_417_without_100(self):
        with running_server(self.root) as port:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
                client.sendall(b"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nExpect: nope\r\n\r\n")
                status, _, _ = read_response(client)
            self.assertTrue(status.startswith("HTTP/1.1 417 "), status)


class DynamicEndpointTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mini-webserver-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def post(self, port, body, extra=b""):
        head = b"POST /api/echo HTTP/1.1\r\nHost: h\r\nContent-Length: %d\r\n" % len(body)
        return request(port, (head + extra + b"\r\n" + body,))

    def test_whoami_json_and_head(self):
        with running_server(self.root, extra_args=("--name", 'a"b')) as port:
            status, headers, body = request(port, (b"GET /api/whoami?x=1 HTTP/1.1\r\nHost: h\r\n\r\n",))
            self.assertEqual(status, "HTTP/1.1 200 OK")
            self.assertEqual(headers["Content-Type"], "application/json")
            self.assertEqual(json.loads(body), {"name": 'a"b', "port": port})
            status, headers, body = request(port, (b"HEAD /api/whoami HTTP/1.1\r\nHost: h\r\n\r\n",))
            self.assertEqual(status, "HTTP/1.1 200 OK")
            self.assertEqual(int(headers["Content-Length"]), len(json.dumps({"name": 'a"b', "port": port}, separators=(",", ":"))) + 1)
            self.assertEqual(body, b"")

    def test_default_name_is_server_port(self):
        with running_server(self.root) as port:
            _, _, body = request(port, (b"GET /api/whoami HTTP/1.1\r\nHost: h\r\n\r\n",))
            self.assertEqual(json.loads(body), {"name": f"server-{port}", "port": port})

    def test_echo_is_byte_exact(self):
        with running_server(self.root) as port:
            for body in (bytes(range(256)) * 40, "한글 🎉".encode(), b""):
                status, headers, got = self.post(port, body)
                self.assertEqual(status, "HTTP/1.1 200 OK")
                self.assertEqual(got, body)
                self.assertEqual(int(headers["Content-Length"]), len(body))
                self.assertEqual(headers["Content-Type"], "application/octet-stream")

    def test_echo_content_type_is_copied(self):
        with running_server(self.root) as port:
            _, headers, _ = self.post(port, b"{}", b"content-type: application/json; charset=x\r\n")
            self.assertEqual(headers["Content-Type"], "application/json; charset=x")

    def test_wrong_methods_get_405_with_allow(self):
        with running_server(self.root) as port:
            for line, allow in ((b"GET /api/echo", "POST"), (b"HEAD /api/echo", "POST"),
                                (b"POST /api/whoami", "GET, HEAD")):
                status, headers, body = request(
                    port, (line + b" HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n",))
                self.assertEqual(status, "HTTP/1.1 405 Method Not Allowed", line)
                self.assertEqual(headers["Allow"], allow)
                if line.startswith(b"HEAD"):
                    self.assertEqual(body, b"")

    def test_other_api_paths_fall_through_to_404(self):
        with running_server(self.root) as port:
            for path in (b"/api/nope", b"/api/echo/", b"/api/"):
                status, _, _ = request(port, (b"GET " + path + b" HTTP/1.1\r\nHost: h\r\n\r\n",))
                self.assertEqual(status, "HTTP/1.1 404 Not Found", path)

    @unittest.skipUnless(shutil.which("curl"), "curl not installed")
    def test_curl_large_post_has_no_continue_delay(self):
        data = Path(self.directory.name) / "big.bin"
        data.write_bytes(b"x" * (1024 * 1024 - 100))
        with running_server(self.root) as port:
            started = time.monotonic()
            out = subprocess.run(
                ["curl", "-s", "-H", "Expect: 100-continue", "--data-binary", f"@{data}",
                 f"http://127.0.0.1:{port}/api/echo"], capture_output=True, timeout=10, check=True)
            self.assertLess(time.monotonic() - started, 0.9)
            self.assertEqual(out.stdout, data.read_bytes())

    def test_name_longer_than_63_bytes_is_argument_error(self):
        for name in ("a" * 64, ""):
            result = subprocess.run([str(BINARY), "--name", name, "--root", str(self.root)],
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 2)
        with running_server(self.root, extra_args=("--name", "n" * 63)):
            pass


def read_one_response(client):
    """One response framed by Content-Length; the connection stays open."""
    data = bytearray()
    while b"\r\n\r\n" not in data:
        chunk = client.recv(65536)
        if not chunk:
            raise AssertionError(f"Closed before a complete head: {bytes(data[:200])!r}")
        data.extend(chunk)
    head, _, body = bytes(data).partition(b"\r\n\r\n")
    lines = head.decode("ascii").split("\r\n")
    headers = dict(line.split(": ", 1) for line in lines[1:])
    body = bytearray(body)
    while len(body) < int(headers["Content-Length"]):
        chunk = client.recv(65536)
        if not chunk:
            raise AssertionError("Closed in the middle of the body")
        body.extend(chunk)
    return lines[0], headers, bytes(body)


@contextmanager
def fake_backend(reply):
    """Backend that records each raw request (head + Content-Length body), answers `reply`
    and closes. Yields (port, received)."""
    listener = socket.create_server(("127.0.0.1", 0))
    listener.settimeout(0.1)
    received = []
    stop = threading.Event()

    def serve():
        while not stop.is_set():
            try:
                conn, _ = listener.accept()
            except socket.timeout:
                continue
            with conn:
                conn.settimeout(5)
                data = bytearray()
                while b"\r\n\r\n" not in data:
                    chunk = conn.recv(65536)
                    if not chunk:
                        break
                    data.extend(chunk)
                head = bytes(data).partition(b"\r\n\r\n")[0]
                length = 0
                for line in head.split(b"\r\n")[1:]:
                    name, _, value = line.partition(b":")
                    if name.strip().lower() == b"content-length":
                        length = int(value)
                while len(data) < len(head) + 4 + length:
                    data.extend(conn.recv(65536))
                received.append(bytes(data))
                conn.sendall(reply)

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    try:
        yield listener.getsockname()[1], received
    finally:
        stop.set()
        thread.join(timeout=3)
        listener.close()


class ProxyTests(unittest.TestCase):
    """One route, one backend (docs/spec.md "Reverse Proxy")."""

    REPLY = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mini-webserver-proxy-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "index.html").write_bytes(b"<h1>front</h1>\n")

    def proxy_to(self, backend_port):
        return running_server(self.root, extra_args=("--route", f"/api/=127.0.0.1:{backend_port}"))

    def test_whoami_and_echo_go_through_the_backend(self):
        with running_server(self.root, extra_args=("--name", "backend-a")) as backend:
            with self.proxy_to(backend) as proxy:
                status, headers, body = request(
                    proxy, (b"GET /api/whoami HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",))
                self.assertEqual(status, OK_STATUS)
                self.assertEqual(headers["Content-Type"], "application/json")
                self.assertEqual(json.loads(body), {"name": "backend-a", "port": backend})
                status, headers, empty = request(
                    proxy, (b"HEAD /api/whoami HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",))
                self.assertEqual((status, empty), (OK_STATUS, b""))
                self.assertEqual(int(headers["Content-Length"]), len(body))
                payload = bytes(range(256)) * 64 + "한글 🎉".encode()
                status, headers, body = request(proxy, (
                    b"POST /api/echo HTTP/1.1\r\nHost: h\r\nConnection: close\r\n"
                    b"Content-Length: %d\r\n\r\n" % len(payload) + payload,))
                self.assertEqual((status, body), (OK_STATUS, payload))
                self.assertEqual(headers["Content-Type"], "application/octet-stream")

    def test_request_is_rewritten_for_the_backend(self):
        # HTTP/1.0 in, HTTP/1.1 out. (A 1.0 client also never gets 100 Continue.)
        raw = (b"POST /api/echo HTTP/1.0\r\nHost: front.example:8080\r\n"
               b"Connection: close, X-Hop\r\nKeep-Alive: timeout=5\r\nProxy-Connection: keep-alive\r\n"
               b"TE: trailers\r\nTrailer: X-T\r\nUpgrade: websocket\r\nX-Hop: secret\r\n"
               b"Expect: 100-continue\r\nX-Forwarded-For: 6.6.6.6\r\nX-Keep: yes\r\n"
               b"Content-Length: 5\r\n\r\nhello")
        with fake_backend(self.REPLY) as (backend, received):
            with self.proxy_to(backend) as proxy:
                status, _, body = request(proxy, (raw,))
        self.assertEqual((status, body), (OK_STATUS, b"ok"))
        self.assertEqual(len(received), 1)
        head, _, sent_body = received[0].partition(b"\r\n\r\n")
        lines = head.decode("ascii").split("\r\n")
        self.assertEqual(lines[0], "POST /api/echo HTTP/1.1")
        names = [line.split(":", 1)[0].lower() for line in lines[1:]]
        for hop in ("keep-alive", "proxy-connection", "te", "trailer", "upgrade", "x-hop", "expect"):
            self.assertNotIn(hop, names)
        for single in ("host", "connection", "x-forwarded-for", "content-length"):
            self.assertEqual(names.count(single), 1, single)
        headers = dict(line.split(": ", 1) for line in lines[1:])
        self.assertEqual(headers["Host"], "front.example:8080")
        self.assertEqual(headers["Connection"], "close")
        self.assertEqual(headers["X-Forwarded-For"], "127.0.0.1")
        self.assertEqual(headers["Content-Length"], "5")
        self.assertEqual(headers["X-Keep"], "yes")
        self.assertEqual(sent_body, b"hello")

    def test_path_and_query_are_forwarded_unchanged(self):
        targets = [b"/api/a%20b/./c?x=1&y=%2F", b"/api/?", b"/api/x?q=a?b&", b"/api/%ED%95%9C"]
        with fake_backend(self.REPLY) as (backend, received):
            with self.proxy_to(backend) as proxy:
                for target in targets:
                    status, _, _ = request(
                        proxy, (b"GET " + target + b" HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",))
                    self.assertEqual(status, OK_STATUS)
        self.assertEqual([r.split(b"\r\n", 1)[0] for r in received],
                         [b"GET " + t + b" HTTP/1.1" for t in targets])

    def test_one_keep_alive_connection_carries_several_requests(self):
        with running_server(self.root, extra_args=("--name", "backend-a")) as backend:
            with self.proxy_to(backend) as proxy:
                with socket.create_connection(("127.0.0.1", proxy), timeout=5) as client:
                    for _ in range(3):
                        client.sendall(b"GET /api/whoami HTTP/1.1\r\nHost: h\r\n\r\n")
                        status, headers, body = read_one_response(client)
                        self.assertEqual(status, OK_STATUS)
                        self.assertEqual(headers["Connection"], "keep-alive")
                        self.assertEqual(json.loads(body)["name"], "backend-a")
                    client.sendall(b"POST /api/echo HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n"
                                   b"Connection: close\r\n\r\nbye")
                    status, headers, body = read_response(client)  # EOF: closed after this one
                self.assertEqual((status, body), (OK_STATUS, b"bye"))
                self.assertEqual(headers["Connection"], "close")

    def test_dead_backend_is_502_and_unrouted_paths_stay_local(self):
        with self.proxy_to(free_port()) as proxy:
            status, headers, body = request(
                proxy, (b"GET /api/whoami HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",))
            self.assertEqual(status, "HTTP/1.1 502 Bad Gateway")
            self.assertEqual(int(headers["Content-Length"]), len(body))
            self.assertEqual(headers["Server"], f"server-{proxy}")
            status, _, body = request(proxy)
            self.assertEqual((status, body), (OK_STATUS, b"<h1>front</h1>\n"))

    def get(self, proxy, path=b"/api/x", version=b"1.1"):
        return request(proxy, (b"GET " + path + b" HTTP/" + version +
                               b"\r\nHost: h\r\nConnection: close\r\n\r\n",))

    def test_silent_backend_is_504_after_three_seconds_and_next_request_works(self):
        listener = socket.create_server(("127.0.0.1", 0))
        self.addCleanup(listener.close)
        with self.proxy_to(listener.getsockname()[1]) as proxy:
            started = time.monotonic()
            status, headers, body = self.get(proxy)
            elapsed = time.monotonic() - started
            self.assertEqual(status, "HTTP/1.1 504 Gateway Timeout")
            self.assertEqual(int(headers["Content-Length"]), len(body))
            self.assertTrue(2.8 <= elapsed < 4.5, elapsed)
            status, _, body = request(proxy)
            self.assertEqual((status, body), (OK_STATUS, b"<h1>front</h1>\n"))

    def test_broken_or_cut_responses_are_502_and_never_retried(self):
        replies = [b"garbage\r\n\r\n",
                   b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc",
                   b"HTTP/1.1 200 OK\r\nContent-Len",
                   b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab",
                   b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nab\r\n0\r\n\r\n",
                   b""]
        for reply in replies:
            with self.subTest(reply=reply), fake_backend(reply) as (backend, received):
                with self.proxy_to(backend) as proxy:
                    status, headers, body = self.get(proxy)
                    self.assertEqual(status, "HTTP/1.1 502 Bad Gateway")
                    self.assertEqual(int(headers["Content-Length"]), len(body))
                self.assertEqual(len(received), 1)

    def test_response_over_8_mib_is_502_and_exactly_8_mib_passes(self):
        limit = 8 * 1024 * 1024
        head = b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n"
        size = limit - len(head % limit)
        exact = head % size + b"x" * size
        self.assertEqual(len(exact), limit)
        with fake_backend(exact) as (backend, _), self.proxy_to(backend) as proxy:
            status, _, body = self.get(proxy)
            self.assertEqual((status, len(body)), (OK_STATUS, size))
        with fake_backend(exact + b"x") as (backend, _), self.proxy_to(backend) as proxy:
            status, _, _ = self.get(proxy)
            self.assertEqual(status, "HTTP/1.1 502 Bad Gateway")

    def test_chunked_reply_reaches_an_http10_client_with_content_length(self):
        reply = (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-T\r\n\r\n"
                 b"3\r\nabc\r\n4;e=1\r\ndefg\r\n0\r\nX-T: v\r\n\r\n")
        with fake_backend(reply) as (backend, _), self.proxy_to(backend) as proxy:
            status, headers, body = self.get(proxy, version=b"1.0")
            self.assertEqual((status, body), (OK_STATUS, b"abcdefg"))
            self.assertEqual(headers["Content-Length"], "7")
            self.assertNotIn("Transfer-Encoding", headers)
            self.assertNotIn("X-T", headers)

    def test_set_cookie_stays_separate(self):
        reply = (b"HTTP/1.1 200 OK\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
                 b"Content-Length: 2\r\n\r\nok")
        with fake_backend(reply) as (backend, _), self.proxy_to(backend) as proxy:
            with socket.create_connection(("127.0.0.1", proxy), timeout=5) as client:
                client.sendall(b"GET /api/x HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n")
                raw = bytearray()
                while chunk := client.recv(65536):
                    raw.extend(chunk)
        head = bytes(raw).partition(b"\r\n\r\n")[0].split(b"\r\n")
        self.assertEqual([l for l in head if l.lower().startswith(b"set-cookie")],
                         [b"Set-Cookie: a=1", b"Set-Cookie: b=2"])

    def test_bodyless_statuses_and_head(self):
        for reply, method in ((b"HTTP/1.1 204 No Content\r\n\r\n", b"GET"),
                              (b"HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\n\r\n", b"GET"),
                              (b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", b"HEAD")):
            with self.subTest(reply=reply), fake_backend(reply) as (backend, _):
                with self.proxy_to(backend) as proxy:
                    status, _, body = request(
                        proxy, (method + b" /api/x HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",))
                    self.assertEqual(body, b"")
                    self.assertEqual(status.split(" ")[1], reply.split(b" ")[1].decode())

    def test_invalid_route_is_argument_error(self):
        for route in ("api/=127.0.0.1:8001", "/api/", "/api/=127.0.0.1", "/api/=127.0.0.1:",
                      "/api/=127.0.0.1:0", "/api/=127.0.0.1:65536", "/api/=:8001",
                      "=127.0.0.1:8001", "/api/=127.0.0.1:80x"):
            with self.subTest(route=route):
                result = subprocess.run([str(BINARY), "--root", str(self.root), "--route", route],
                                        capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(b"usage:", result.stderr)
                assert_no_sanitizer_errors(result.stderr.decode())


def wait_exit(process, seconds=5):
    try:
        return process.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        raise AssertionError(f"Server did not exit within {seconds}s of the signal") from None


def wait_refused(port, seconds=5):
    """Until the listener is gone (shutdown has begun); bounded, no fixed sleep."""
    deadline = time.monotonic() + seconds
    while True:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
        except ConnectionRefusedError:
            return
        except OSError:
            pass
        if time.monotonic() >= deadline:
            raise AssertionError(f"Listener still open {seconds}s after the signal")
        time.sleep(0.02)


class ShutdownTests(unittest.TestCase):
    """Signals and graceful shutdown (docs/spec.md "시그널과 종료")."""

    BIG = 8 * 1024 * 1024  # well above the loopback socket buffers

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mini-webserver-shutdown-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "index.html").write_bytes(b"x" * self.BIG)

    def slow_reader(self, port):
        """Sends GET / and reads only the response head, so the worker is mid-response."""
        client = socket.socket()
        client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 * 1024)
        client.settimeout(10)
        client.connect(("127.0.0.1", port))
        client.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
        data = bytearray()
        while b"\r\n\r\n" not in data:
            chunk = client.recv(4096)
            if not chunk:
                raise AssertionError("Closed before the response head")
            data.extend(chunk)
        return client, bytes(data)

    def test_sigint_and_sigterm_exit_zero_quickly(self):
        for number in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=number.name):
                with server_process(self.root) as (process, port):
                    process.send_signal(number)
                    self.assertEqual(wait_exit(process), 0)
                    with self.assertRaises(ConnectionRefusedError):
                        socket.create_connection(("127.0.0.1", port), timeout=2).close()

    def test_disconnect_during_a_large_response_keeps_serving(self):
        with running_server(self.root) as port:
            for _ in range(5):
                client, _ = self.slow_reader(port)  # the worker is blocked writing the body
                client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                client.close()
            status, _, body = request(port)
            self.assertEqual((status, len(body)), (OK_STATUS, self.BIG))

    def test_response_in_flight_completes_after_sigterm(self):
        with server_process(self.root) as (process, port):
            client, received = self.slow_reader(port)
            with client:
                process.send_signal(signal.SIGTERM)
                wait_refused(port)  # shutdown began while the response is still unread
                self.assertIsNone(process.poll(), "Server left before its response was read")
                head, _, body = received.partition(b"\r\n\r\n")
                length = int(dict(line.split(": ", 1)
                                  for line in head.decode("ascii").split("\r\n")[1:])
                             ["Content-Length"])
                body = bytearray(body)
                while True:
                    chunk = client.recv(65536)
                    if not chunk:
                        break
                    body.extend(chunk)
            self.assertEqual(len(body), length)
            self.assertEqual(wait_exit(process), 0)

    def test_idle_keep_alive_connection_is_closed_at_shutdown(self):
        with running_server(self.root) as backend:
            route = f"/api/=127.0.0.1:{backend}"
            with server_process(self.root, extra_args=("--route", route)) as (process, proxy):
                with socket.create_connection(("127.0.0.1", proxy), timeout=5) as client:
                    client.sendall(b"GET /api/whoami HTTP/1.1\r\nHost: h\r\n\r\n")
                    self.assertEqual(read_one_response(client)[0], OK_STATUS)
                    process.send_signal(signal.SIGTERM)
                    # Well under the 5 s idle timeout: the server closes it on its own.
                    client.settimeout(3)
                    self.assertEqual(client.recv(1), b"")
                self.assertEqual(wait_exit(process), 0)


class StaticFileTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mw-static-", dir="/tmp")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "index.html").write_bytes(b"root index")
        (self.root / "docs").mkdir()
        (self.root / "docs" / "index.html").write_bytes(b"docs index")
        (self.root / "docs" / "한 글.CSS").write_bytes(b"body { color: red; }\n")
        (self.root / "empty").mkdir()

    def fetch(self, port, target, method=b"GET"):
        return request(port, (method + b" " + target + b" HTTP/1.1\r\nHost: h\r\n"
                              b"Connection: close\r\nContent-Length: 0\r\n\r\n",))

    def test_directories_queries_and_encoded_filenames(self):
        with running_server(self.root) as port:
            for target, expected, mime in (
                (b"/?v=1", b"root index", "text/html; charset=utf-8"),
                (b"/docs", b"docs index", "text/html; charset=utf-8"),
                (b"/docs/", b"docs index", "text/html; charset=utf-8"),
                (b"/docs//index.html?x=%zz", b"docs index", "text/html; charset=utf-8"),
                (b"/docs/%ED%95%9C%20%EA%B8%80.CSS?v=2", b"body { color: red; }\n",
                 "text/css; charset=utf-8"),
            ):
                with self.subTest(target=target):
                    status, headers, body = self.fetch(port, target)
                    self.assertEqual((status, body), (OK_STATUS, expected))
                    self.assertEqual(headers["Content-Type"], mime)
                    self.assertEqual(int(headers["Content-Length"]), len(body))

    def test_errors_and_path_escape_attempts(self):
        outside = self.root.parent / (self.root.name + "-outside.txt")
        outside.write_bytes(b"secret")
        self.addCleanup(outside.unlink)
        (self.root / "link").symlink_to(outside)
        (self.root / "dir-link").symlink_to(self.root / "docs", target_is_directory=True)
        (self.root / "dangling").symlink_to(self.root / "missing")
        (self.root / "symlink-index").mkdir()
        (self.root / "symlink-index" / "index.html").symlink_to(outside)
        (self.root / "directory-index").mkdir()
        (self.root / "directory-index" / "index.html").mkdir()
        os.mkfifo(self.root / "fifo")
        special = socket.socket(socket.AF_UNIX)
        self.addCleanup(special.close)
        special.bind(str(self.root / "socket"))
        (self.root / "socket-index").mkdir()
        index_socket = socket.socket(socket.AF_UNIX)
        self.addCleanup(index_socket.close)
        index_socket.bind(str(self.root / "socket-index" / "index.html"))
        with running_server(self.root) as port:
            for target, code in (
                (b"/missing", 404), (b"/index.html/child", 404), (b"/index.html/", 404),
                (b"/empty", 403), (b"/empty/", 403), (b"/link", 403), (b"/dir-link/index.html", 403),
                (b"/dangling", 403), (b"/symlink-index/", 403), (b"/directory-index/", 403),
                (b"/fifo", 403), (b"/socket", 403), (b"/socket-index/", 403), (b"/../secret", 403), (b"/./index.html", 403),
                (b"/%2e%2e/secret", 403), (b"/docs%2f..%2findex.html", 403),
                (b"/%00", 403), (b"/%1f", 403), (b"/%7f", 403),
                (b"/%", 400), (b"/%0", 400), (b"/%GG", 400),
            ):
                with self.subTest(target=target):
                    status, headers, body = self.fetch(port, target)
                    self.assertTrue(status.startswith(f"HTTP/1.1 {code} "), status)
                    self.assertEqual(int(headers["Content-Length"]), len(body))
                    self.assertIn(str(code).encode(), body)

    @unittest.skipIf(os.geteuid() == 0, "root bypasses file permissions")
    def test_unreadable_file_and_directory_are_403(self):
        denied = self.root / "denied.txt"
        denied.write_bytes(b"secret")
        denied.chmod(0)
        self.addCleanup(denied.chmod, 0o600)
        locked = self.root / "locked"
        locked.mkdir()
        (locked / "a.txt").write_bytes(b"secret")
        locked.chmod(0)
        self.addCleanup(locked.chmod, 0o700)
        with running_server(self.root) as port:
            for target in (b"/denied.txt", b"/locked/a.txt"):
                status, _, _ = self.fetch(port, target)
                self.assertEqual(status, "HTTP/1.1 403 Forbidden")

    def test_head_matches_get_and_post_has_allow(self):
        with running_server(self.root) as port:
            for target in (b"/", b"/docs/", b"/docs/index.html", b"/missing", b"/empty/"):
                get_status, get_headers, _ = self.fetch(port, target)
                head_status, head_headers, body = self.fetch(port, target, b"HEAD")
                self.assertEqual(head_status, get_status)
                for key in ("Content-Type", "Content-Length", "Connection", "Server"):
                    self.assertEqual(head_headers[key], get_headers[key])
                self.assertIsNotNone(parsedate_to_datetime(head_headers["Date"]))
                self.assertEqual(body, b"")
            status, headers, body = self.fetch(port, b"/docs/index.html", b"POST")
            self.assertEqual(status, "HTTP/1.1 405 Method Not Allowed")
            self.assertEqual(headers["Allow"], "GET, HEAD")
            self.assertEqual(int(headers["Content-Length"]), len(body))

    def test_binary_multimegabyte_and_unknown_mime(self):
        content = bytes(range(256)) * 20481
        (self.root / "docs" / "large.bin").write_bytes(content)
        with running_server(self.root) as port:
            status, headers, body = self.fetch(port, b"/docs/large.bin")
            self.assertEqual((status, body), (OK_STATUS, content))
            self.assertEqual(int(headers["Content-Length"]), len(content))
            self.assertEqual(headers["Content-Type"], "application/octet-stream")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    options, remaining = parser.parse_known_args()
    BINARY = options.binary.resolve()
    unittest.main(argv=[__file__, *remaining], verbosity=2)
