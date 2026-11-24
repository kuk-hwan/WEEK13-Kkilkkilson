"""Process/TCP contract tests; no HTTP framework or external dependency."""
import argparse
import json
import shutil
from contextlib import contextmanager
from email.utils import parsedate_to_datetime
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
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
def running_server(root, port=None, extra=()):
    port = free_port() if port is None else port
    with tempfile.TemporaryFile(mode="w+b") as log:
        process = subprocess.Popen(
            [str(BINARY), "--host", "127.0.0.1", "--port", str(port), "--root", str(root), *extra],
            stdout=subprocess.DEVNULL, stderr=log,
        )
        try:
            deadline = time.monotonic() + 10
            while True:
                if process.poll() is not None:
                    log.seek(0)
                    raise AssertionError(f"Server exited during startup: {log.read().decode()}")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        break
                except OSError:
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
            for line in (b"GET /missing HTTP/1.1", b"GET /?v=1 HTTP/1.1"):
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
        with running_server(self.root, extra=("--name", 'a"b')) as port:
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
        with running_server(self.root, extra=("--name", "n" * 63)):
            pass


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    options, remaining = parser.parse_known_args()
    BINARY = options.binary.resolve()
    unittest.main(argv=[__file__, *remaining], verbosity=2)
