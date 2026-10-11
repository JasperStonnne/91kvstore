#!/usr/bin/env python3
"""启动独立的 KVStore，检查基础命令和现有流水线测试。"""

import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import uuid


PROJECT_DIR = Path(__file__).resolve().parent.parent
KVSTORE_BIN = PROJECT_DIR / "bin" / "kvstore"
PIPELINING_TEST = PROJECT_DIR / "tests" / "test_pipelining.py"


class TestFailure(Exception):
    """测试条件未满足。"""


def choose_port():
    """让操作系统分配当前空闲的 TCP 端口。"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("0.0.0.0", 0))
        return probe.getsockname()[1]


def stop_server(process):
    """只停止本脚本启动的进程，并等待它退出。"""
    if process is None or process.poll() is not None:
        return

    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def wait_for_server(process, port):
    """最多等待 10 秒，确认服务能接受连接。"""
    deadline = time.monotonic() + 10

    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise TestFailure(
                f"KVStore exited before becoming ready (exit {process.returncode})"
            )

        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                if process.poll() is None:
                    return
        except OSError:
            time.sleep(0.1)

    raise TestFailure(f"KVStore did not listen on port {port} within 10 seconds")


def test_basic_commands(port):
    """用当前项目的文本命令和回复格式检查写、读、改、删。"""
    key = "basic_" + uuid.uuid4().hex
    cases = [
        ("initially missing", f"GET {key}", b"NO EXIST\r\n"),
        ("write", f"SET {key} first", b"OK\r\n"),
        ("read", f"GET {key}", b"first\r\n"),
        ("modify", f"MOD {key} second", b"OK\r\n"),
        ("read modified value", f"GET {key}", b"second\r\n"),
        ("delete", f"DEL {key}", b"OK\r\n"),
        ("missing after delete", f"GET {key}", b"NO EXIST\r\n"),
    ]

    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        with connection.makefile("rb") as replies:
            for name, request, expected in cases:
                try:
                    connection.sendall((request + "\r\n").encode("ascii"))
                    actual = replies.readline()
                except OSError as exc:
                    raise TestFailure(f"basic commands / {name}: {exc}") from exc

                if actual != expected:
                    raise TestFailure(
                        f"basic commands / {name}: "
                        f"expected {expected!r}, got {actual!r}"
                    )
                print(f"[PASS] basic commands / {name}")


def test_pipelining(port):
    """运行仓库原有的两项流水线测试，并检查其退出码。"""
    try:
        completed = subprocess.run(
            [sys.executable, str(PIPELINING_TEST), str(port)],
            timeout=15,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise TestFailure("pipelining test exceeded 15 seconds") from exc

    if completed.returncode != 0:
        raise TestFailure(
            f"pipelining test exited with status {completed.returncode}"
        )


def main():
    if not os.access(KVSTORE_BIN, os.X_OK):
        print(f"[FAIL] missing executable: {KVSTORE_BIN}; run make first", file=sys.stderr)
        return 2

    # KVStore 的 AOF、Snapshot 和日志都留在这个测试专用目录。
    test_dir = Path(tempfile.mkdtemp(prefix="91kv-standalone-test-"))
    log_path = test_dir / "kvstore.log"
    process = None
    port = None
    result = 1

    try:
        # 端口从操作系统的空闲端口中选；若启动时恰好被抢占，重试。
        for attempt in range(1, 4):
            port = choose_port()
            with log_path.open("ab") as log:
                log.write(f"\n--- start attempt {attempt}, port {port} ---\n".encode())
                log.flush()
                process = subprocess.Popen(
                    [str(KVSTORE_BIN), str(port)],
                    cwd=test_dir,
                    stdin=subprocess.DEVNULL,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                )

            try:
                wait_for_server(process, port)
                break
            except TestFailure:
                stop_server(process)
                process = None
                if attempt == 3:
                    raise

        print(f"[INFO] standalone KVStore ready on port {port}")
        test_basic_commands(port)
        test_pipelining(port)
        result = 0

    except (OSError, TestFailure) as exc:
        print(f"[FAIL] standalone integration: {exc}", file=sys.stderr)

    finally:
        stop_server(process)

        if result == 0:
            try:
                shutil.rmtree(test_dir)
            except OSError as exc:
                print(f"[FAIL] could not clean test directory: {exc}", file=sys.stderr)
                result = 1

        if result != 0:
            print(f"[INFO] test files and log kept at: {test_dir}", file=sys.stderr)
            print(f"[INFO] server port: {port}", file=sys.stderr)
            if log_path.is_file():
                print("----- last 60 server log lines -----", file=sys.stderr)
                for line in log_path.read_text(errors="replace").splitlines()[-60:]:
                    print(line, file=sys.stderr)

    return result


if __name__ == "__main__":
    sys.exit(main())