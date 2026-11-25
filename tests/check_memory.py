"""Run the real proxy and backend under Memcheck and validate both shutdown reports."""

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import re
import signal
import socket
import tempfile

from integration import OK_STATUS, read_one_response, request, server_process


@contextmanager
def checked_server(binary, root, report, extra_args=()):
    command = (
        "valgrind", "--leak-check=full", "--show-leak-kinds=all",
        "--errors-for-leak-kinds=all", "--track-fds=yes", "--error-exitcode=66",
        # Reuse stderr so Valgrind 3.22 does not count its own log FD as open.
        "--log-fd=2",
    )
    with server_process(root, extra_args=extra_args, binary=binary,
                        wrapper=command, stop_signal=signal.SIGINT,
                        log_path=report) as (process, port):
        yield port
    log = report.read_text(encoding="utf-8", errors="replace")
    descriptors = re.search(r"FILE DESCRIPTORS: (\d+) open \((\d+) (?:std|inherited)\) at exit\.", log)
    heap_empty = re.search(r"in use at exit: 0 bytes in 0 blocks\b", log)
    errors_zero = re.search(r"ERROR SUMMARY: 0 errors\b", log)
    if process.returncode != 0 or not heap_empty or not errors_zero or (
        not descriptors or descriptors.groups() != ("3", "3")
    ):
        raise AssertionError(f"Valgrind failed for {report} (exit {process.returncode})\n{log}")
    print(f"{report}: 0 leaked bytes, 0 memory errors, 0 extra FDs")


def check_memory(binary, reports):
    with tempfile.TemporaryDirectory(prefix="mini-webserver-valgrind-") as directory:
        root = Path(directory)
        content = b"static file\n"
        (root / "index.html").write_bytes(content)
        with checked_server(binary, root, reports / "backend.log",
                            ("--name", "valgrind-backend")) as backend:
            route = ("--route", f"/api/=127.0.0.1:{backend}")
            with checked_server(binary, root, reports / "proxy.log", route) as proxy:
                status, _, body = request(proxy)
                if (status, body) != (OK_STATUS, content):
                    raise AssertionError(f"Unexpected static response: {status!r}, {body!r}")

                with socket.create_connection(("127.0.0.1", proxy), timeout=8) as client:
                    client.sendall(b"GET /api/whoami HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    status, headers, body = read_one_response(client)
                    if (status != OK_STATUS or headers.get("Connection") != "keep-alive"
                            or json.loads(body) != {"name": "valgrind-backend", "port": backend}):
                        raise AssertionError(f"Unexpected routed GET response: {status!r}, {body!r}")
                    client.sendall(b"POST /api/echo HTTP/1.1\r\nHost: localhost\r\n"
                                   b"Content-Length: 4\r\nConnection: close\r\n\r\necho")
                    status, _, body = read_one_response(client)
                    if (status, body) != (OK_STATUS, b"echo"):
                        raise AssertionError(f"Unexpected routed POST response: {status!r}, {body!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--reports", required=True, type=Path)
    args = parser.parse_args()
    reports = args.reports.resolve()
    reports.mkdir(parents=True, exist_ok=True)
    check_memory(args.binary.resolve(), reports)


if __name__ == "__main__":
    main()
