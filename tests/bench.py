"""Run a fixed ApacheBench matrix against temporary loopback-only servers."""

import argparse
import csv
from datetime import datetime, timezone
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile

from integration import server_process


CONCURRENCY_LEVELS = (1, 10, 50, 100)
WORKER_COUNTS = (8, 64)
QUEUE_CAPACITY = 256
BACKEND_WORKERS = 8
CSV_FIELDS = (
    "measured_at_utc", "os", "cpu", "logical_cpus", "workers", "backend_workers",
    "queue_capacity", "endpoint", "concurrency", "keep_alive", "requests",
    "completed", "keep_alive_responses", "requests_per_second", "mean_request_ms",
    "p95_ms", "failed", "non_2xx",
)


def cpu_model():
    cpu_info = Path("/proc/cpuinfo")
    if cpu_info.exists():
        model = re.search(r"^model name\s*:\s*(.+)$", cpu_info.read_text(), flags=re.MULTILINE)
        if model:
            return model[1]
    if platform.system() == "Darwin":
        return subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip()
    return platform.processor() or platform.machine()


def result_value(output, label, default=None):
    match = re.search(rf"^{re.escape(label)}\s*([\d.]+)", output, flags=re.MULTILINE)
    if match:
        return match[1]
    if default is not None:
        return default
    raise ValueError(f"Missing ApacheBench metric: {label}\n{output}")


def measure(port, target, requests, concurrency, keep_alive):
    command = ["ab", "-r", "-s", "15", "-n", str(requests), "-c", str(concurrency)]
    if keep_alive:
        command.append("-k")
    command.append(f"http://127.0.0.1:{port}{target}")
    environment = dict(os.environ, LC_ALL="C")
    result = subprocess.run(command, env=environment, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, timeout=120)
    if result.returncode:
        raise RuntimeError(f"ApacheBench failed for {target} (exit {result.returncode})\n"
                           f"{result.stdout}\n{result.stderr}")
    output = result.stdout
    mean = re.search(r"^Total:\s+\d+\s+(\d+)", output, flags=re.MULTILINE)
    if not mean:
        raise ValueError(f"Missing ApacheBench connection time summary\n{output}")
    return {
        "completed": result_value(output, "Complete requests:"),
        "keep_alive_responses": result_value(output, "Keep-Alive requests:", default="0"),
        "requests_per_second": result_value(output, "Requests per second:"),
        "mean_request_ms": mean[1],
        "p95_ms": result_value(output, "  95%"),
        "failed": result_value(output, "Failed requests:"),
        "non_2xx": result_value(output, "Non-2xx responses:", default="0"),
    }


def benchmark(binary, requests, report):
    report.parent.mkdir(parents=True, exist_ok=True)
    environment = {
        "measured_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "os": platform.platform(),
        "cpu": cpu_model(),
        "logical_cpus": os.cpu_count(),
        "backend_workers": BACKEND_WORKERS,
        "queue_capacity": QUEUE_CAPACITY,
        "requests": requests,
    }
    with tempfile.TemporaryDirectory(prefix="mini-webserver-bench-") as directory, \
            report.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
        writer.writeheader()
        root = Path(directory)
        payload = b"x" * 1024
        (root / "index.html").write_bytes(payload)
        (root / "proxy").mkdir()
        (root / "proxy" / "index.html").write_bytes(payload)
        backend_options = ("--workers", str(BACKEND_WORKERS), "--queue", str(QUEUE_CAPACITY))
        with server_process(root, binary=binary, extra_args=backend_options) as (_, backend_port):
            route = f"/proxy/=127.0.0.1:{backend_port}"
            for workers in WORKER_COUNTS:
                extra_args = ("--workers", str(workers), "--queue", str(QUEUE_CAPACITY), "--route", route)
                with server_process(root, binary=binary, extra_args=extra_args) as (_, port):
                    for endpoint, target in (("static", "/index.html"), ("proxy", "/proxy/index.html")):
                        for concurrency in CONCURRENCY_LEVELS:
                            for keep_alive in (False, True):
                                metrics = measure(port, target, requests, concurrency, keep_alive)
                                writer.writerow(dict(environment, workers=workers, endpoint=endpoint,
                                                     concurrency=concurrency, keep_alive=int(keep_alive),
                                                     **metrics))
                                stream.flush()
                                print(f"{workers=} {endpoint} c={concurrency} keep-alive={keep_alive} "
                                      f"req/s={metrics['requests_per_second']} "
                                      f"failed={metrics['failed']} non-2xx={metrics['non_2xx']}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--requests", required=True, type=int)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    if args.requests < max(CONCURRENCY_LEVELS):
        parser.error(f"--requests must be at least {max(CONCURRENCY_LEVELS)}")
    report = args.report.resolve()
    benchmark(args.binary.resolve(), args.requests, report)
    print(f"Benchmark report: {report}")


if __name__ == "__main__":
    main()
