"""Process/TCP contract tests; no HTTP framework or external dependency."""
import argparse
from contextlib import contextmanager
from email.utils import parsedate_to_datetime
import os
from pathlib import Path
import select
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
            yield port
            if process.poll() is not None:
                raise AssertionError(f"Server stopped unexpectedly: {process.returncode}")
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

    def test_other_requests_are_404_in_bootstrap(self):
        with running_server(self.root) as port:
            for line in (b"GET /missing HTTP/1.1", b"POST / HTTP/1.1", b"GET /?v=1 HTTP/1.1"):
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
            status, headers, _ = request(port, (b"G" * 16384,))
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    options, remaining = parser.parse_known_args()
    BINARY = options.binary.resolve()
    unittest.main(argv=[__file__, *remaining], verbosity=2)
