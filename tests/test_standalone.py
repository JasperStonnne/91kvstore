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


def wait_for_server(process, port, host="127.0.0.1"):
    """最多等待 10 秒，确认指定 IP:端口能接受连接。"""
    deadline = time.monotonic() + 10

    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise TestFailure(
                f"KVStore exited before becoming ready (exit {process.returncode})"
            )

        try:
            with socket.create_connection((host, port), timeout=0.2):
                if process.poll() is None:
                    return
        except OSError:
            time.sleep(0.1)

    raise TestFailure(
    f"KVStore did not listen on {host}:{port} within 10 seconds"
)

def test_bind_ip(port):
    """只监听 127.0.0.1；同一端口不能从另一个本机地址连入。"""
    other_ip = "127.0.0.2"

    try:
        connection = socket.create_connection((other_ip, port), timeout=1)
    except ConnectionRefusedError:
        # 预期结果：127.0.0.2 上没有这个监听器。
        print("[PASS] bind_ip restricts listener to 127.0.0.1")
        return
    except OSError as exc:
        # 超时、网络不可达等不是我们要验证的“明确拒绝”。
        raise TestFailure(
            f"bind_ip check on {other_ip}:{port} failed unexpectedly: {exc}"
        ) from exc

    # 能连上表示服务可能仍绑定了 0.0.0.0，配置 IP 没真正生效。
    connection.close()
    raise TestFailure(
        f"bind_ip ignored: {other_ip}:{port} accepted a connection"
    )

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

def test_other_log_level(test_dir, level):
    """用独立服务验证 ERROR 或 DEBUG 的实际运行效果。"""
    run_dir = test_dir / f"log-{level}"
    run_dir.mkdir()
    config_path = run_dir / "server.conf"

    # 端口可能在选择后被别的进程抢占，最多尝试三次。
    for attempt in range(1, 4):
        port = choose_port()
        log_path = run_dir / f"attempt-{attempt}.log"
        config_path.write_text(
            "role = standalone\n"
            "bind_ip = 127.0.0.1\n"
            f"service_port = {port}\n"
            f"log_level = {level}\n"
            "persistence_mode = legacy_hybrid\n",
            encoding="utf-8",
        )

        with log_path.open("wb") as log:
            process = subprocess.Popen(
                [str(KVSTORE_BIN), str(config_path)],
                cwd=run_dir,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
            )

        try:
            wait_for_server(process, port)
        except TestFailure as exc:
            stop_server(process)
            if attempt == 3:
                raise TestFailure(
                    f"{level}-level server did not start: {exc}; log: {log_path}"
                ) from exc
            continue

        try:
            # 两种日志级别下服务都必须照常处理命令。
            with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
                connection.settimeout(3)
                connection.sendall(b"GET log_level_probe\r\n")
                with connection.makefile("rb") as replies:
                    response = replies.readline()
            if response != b"NO EXIST\r\n":
                raise TestFailure(
                    f"{level}-level server replied {response!r}; log: {log_path}"
                )
        finally:
            stop_server(process)

        output = log_path.read_text(errors="replace")
        listener_message = f"listen port : {port}"
        debug_message = "client accepted fd="

        if level == "error":
            if listener_message in output or debug_message in output:
                raise TestFailure(
                    f"log_level=error printed INFO or DEBUG; log: {log_path}"
                )
            print("[PASS] log_level=error hides INFO/DEBUG; commands still work")
        elif level == "debug":
            if listener_message not in output or debug_message not in output:
                raise TestFailure(
                    f"log_level=debug missing INFO or DEBUG; log: {log_path}"
                )
            print("[PASS] log_level=debug shows INFO/DEBUG; commands still work")

        return

def test_invalid_config_startup(test_dir):
    """错误配置必须报错退出，不能留下客户端监听端口。"""
    run_dir = test_dir / "invalid-config"
    run_dir.mkdir()
    config_path = run_dir / "invalid.conf"
    log_path = run_dir / "server.log"
    port = choose_port()

    # 端口本身合法，只把日志级别写错；这样才能检查该端口未被监听。
    config_path.write_text(
        "role = standalone\n"
        "bind_ip = 127.0.0.1\n"
        f"service_port = {port}\n"
        "log_level = verbose\n"
        "persistence_mode = legacy_hybrid\n",
        encoding="utf-8",
    )

    try:
        with log_path.open("wb") as log:
            completed = subprocess.run(
                [str(KVSTORE_BIN), str(config_path)],
                cwd=run_dir,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=3,
                check=False,
            )
    except subprocess.TimeoutExpired as exc:
        raise TestFailure(
            f"KVStore kept running with invalid config; log: {log_path}"
        ) from exc

    output = log_path.read_text(errors="replace")
    if completed.returncode == 0:
        raise TestFailure(f"invalid config was accepted; log: {log_path}")
    if str(config_path) not in output or "invalid log_level" not in output:
        raise TestFailure(
            f"invalid config error was not clear: {output!r}; log: {log_path}"
        )
    if "listen port :" in output:
        raise TestFailure(f"invalid config started a listener; log: {log_path}")

    # 进程已经退出；确认它没有留下占用目标地址和端口的监听者。
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(("127.0.0.1", port))
    except OSError as exc:
        raise TestFailure(
            f"port {port} remained unavailable after invalid config; "
            f"log: {log_path}: {exc}"
        ) from exc

    print("[PASS] invalid config rejected before listening")

def main():
    if not os.access(KVSTORE_BIN, os.X_OK):
        print(f"[FAIL] missing executable: {KVSTORE_BIN}; run make first", file=sys.stderr)
        return 2

    # KVStore 的 AOF、Snapshot 和日志都留在这个测试专用目录。
    test_dir = Path(tempfile.mkdtemp(prefix="91kv-standalone-test-"))
    log_path = test_dir / "kvstore.log"
    config_path = test_dir / "standalone.conf"
    process = None
    port = None
    result = 1

    try:
        # 端口从操作系统的空闲端口中选；若启动时恰好被抢占，重试。
        for attempt in range(1, 4):
            port = choose_port()
        # 每次尝试都把选中的端口写进本测试自己的配置文件。
        # 配置与 AOF、Snapshot 都在 test_dir，成功时一并清理。
            config_path.write_text(
                "role = standalone\n"
                "bind_ip = 127.0.0.1\n"
                f"service_port = {port}\n"
                "log_level = info\n"
                "persistence_mode = legacy_hybrid\n",
                encoding="utf-8",
            )
            with log_path.open("ab") as log:
                log.write(f"\n--- start attempt {attempt}, port {port} ---\n".encode())
                log.flush()
                process = subprocess.Popen(
                    [str(KVSTORE_BIN), str(config_path)],
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
        # INFO 级别必须保留正常启动日志；服务端日志函数会立即刷新。
        expected_log = f"listen port : {port}"
        if expected_log not in log_path.read_text(errors="replace"):
            raise TestFailure(
                f"info log missing: expected {expected_log!r} in {log_path}"
            )
        print("[PASS] log_level=info shows listener message")
        # readiness 检查已经建立过连接；INFO 下不应记录 DEBUG 消息。
        if "client accepted fd=" in log_path.read_text(errors="replace"):
            raise TestFailure("log_level=info unexpectedly printed DEBUG message")
        print("[PASS] log_level=info hides DEBUG")
        test_basic_commands(port)
        test_bind_ip(port)
        test_pipelining(port)
        test_other_log_level(test_dir, "error")
        test_other_log_level(test_dir, "debug")
        test_invalid_config_startup(test_dir)
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