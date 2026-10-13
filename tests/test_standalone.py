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
    """用 RESP2 请求和回复检查写、读、改、删。"""
    key = ("basic_" + uuid.uuid4().hex).encode("ascii")
    cases = [
        ("initially missing", (b"GET", key), b"$-1\r\n"),
        ("write", (b"SET", key, b"first"), b"+OK\r\n"),
        ("read", (b"GET", key), b"$5\r\nfirst\r\n"),
        ("modify", (b"MOD", key, b"second"), b"+OK\r\n"),
        ("read modified value", (b"GET", key), b"$6\r\nsecond\r\n"),
        ("delete", (b"DEL", key), b":1\r\n"),
        ("missing after delete", (b"GET", key), b"$-1\r\n"),
    ]

    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        with connection.makefile("rb") as replies:
            for name, fields, expected in cases:
                request = b"*" + str(len(fields)).encode("ascii") + b"\r\n"
                for field in fields:
                    request += (
                        b"$" + str(len(field)).encode("ascii")
                        + b"\r\n" + field + b"\r\n"
                    )

                try:
                    connection.sendall(request)
                    first_line = replies.readline()
                    if first_line.startswith(b"$") and first_line != b"$-1\r\n":
                        value_length = int(first_line[1:-2])
                        actual = first_line + replies.read(value_length + 2)
                    else:
                        actual = first_line
                except (OSError, ValueError) as exc:
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
                connection.sendall(b"*2\r\n$3\r\nGET\r\n$15\r\nlog_level_probe\r\n")
                with connection.makefile("rb") as replies:
                    response = replies.readline()
            if response != b"$-1\r\n":
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

def test_oversized_bulk_header(port):
    """只发送超大长度声明，服务不能无限等待后续内容。"""
    request = b"*2\r\n$3\r\nGET\r\n$1000000000\r\n"

    with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
        connection.settimeout(2)
        connection.sendall(request)
        with connection.makefile("rb") as replies:
            try:
                reply = replies.readline()
            except OSError as exc:
                raise TestFailure(
                    f"oversized Bulk header received no timely reply: {exc}"
                ) from exc

    if not reply.startswith(b"-ERR ") or not reply.endswith(b"\r\n"):
        raise TestFailure(f"oversized Bulk header returned {reply!r}")
    print("[PASS] oversized Bulk header rejected promptly")

def test_resp2_ping(port):
    """RESP2 PING 应返回 Simple String，而不是旧文本。"""
    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        connection.sendall(b"*1\r\n$4\r\nPING\r\n")
        with connection.makefile("rb") as replies:
            actual = replies.readline()

    if actual != b"+PONG\r\n":
        raise TestFailure(f"RESP2 PING: expected b'+PONG\\r\\n', got {actual!r}")
    print("[PASS] RESP2 PING")

def test_resp2_missing_get(port):
    """RESP2 GET 找不到 key 时返回 Nil Bulk String。"""
    key = ("missing_" + uuid.uuid4().hex).encode("ascii")
    request = b"*2\r\n$3\r\nGET\r\n$" + str(len(key)).encode("ascii")
    request += b"\r\n" + key + b"\r\n"

    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        connection.sendall(request)
        with connection.makefile("rb") as replies:
            actual = replies.readline()

    if actual != b"$-1\r\n":
        raise TestFailure(f"RESP2 missing GET: expected nil, got {actual!r}")
    print("[PASS] RESP2 missing GET returns nil")

def test_resp2_write_commands(port):
    key = ("resp_set_" + uuid.uuid4().hex).encode("ascii")
    request = (
        b"*3\r\n$3\r\nSET\r\n"
        + b"$" + str(len(key)).encode("ascii") + b"\r\n" + key + b"\r\n"
        + b"$1\r\nv\r\n"
    )

    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        connection.sendall(request)
        with connection.makefile("rb") as replies:
            actual = replies.readline()

    if actual != b"+OK\r\n":
        raise TestFailure(f"RESP2 SET: expected b'+OK\\r\\n', got {actual!r}")
    print("[PASS] RESP2 SET returns Simple String")
    key_field = b"$" + str(len(key)).encode("ascii") + b"\r\n" + key + b"\r\n"
    duplicate_request = b"*3\r\n$3\r\nSET\r\n" + key_field + b"$1\r\nx\r\n"
    get_request = b"*2\r\n$3\r\nGET\r\n" + key_field

    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        with connection.makefile("rb") as replies:
            connection.sendall(duplicate_request)
            duplicate_reply = replies.readline()

            connection.sendall(get_request)
            value_header = replies.readline()
            value_body = replies.readline()

    if duplicate_reply != b"-ERR key already exists\r\n":
        raise TestFailure(f"duplicate SET returned {duplicate_reply!r}")
    if (value_header, value_body) != (b"$1\r\n", b"v\r\n"):
        raise TestFailure(
            f"duplicate SET changed existing value: {value_header!r} {value_body!r}"
        )
    print("[PASS] duplicate RESP2 SET returns Error and preserves value")
    mod_request = b"*3\r\n$3\r\nMOD\r\n" + key_field + b"$1\r\nx\r\n"
    missing_key = key + b"_missing"
    missing_field = (
        b"$" + str(len(missing_key)).encode("ascii")
        + b"\r\n" + missing_key + b"\r\n"
    )
    missing_mod_request = (
        b"*3\r\n$3\r\nMOD\r\n" + missing_field + b"$1\r\nx\r\n"
    )

    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        connection.sendall(mod_request + get_request + missing_mod_request)
        with connection.makefile("rb") as replies:
            mod_reply = replies.readline()
            value_header = replies.readline()
            value_body = replies.readline()
            missing_reply = replies.readline()

    if (mod_reply, value_header, value_body, missing_reply) != (
        b"+OK\r\n", b"$1\r\n", b"x\r\n", b"-ERR key not found\r\n"
    ):
        raise TestFailure(
            f"RESP2 MOD replies: {mod_reply!r}, {value_header!r}, "
            f"{value_body!r}, {missing_reply!r}"
        )
    print("[PASS] RESP2 MOD succeeds or reports missing key")
    delete_request = b"*2\r\n$3\r\nDEL\r\n" + key_field

    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        connection.sendall(delete_request + delete_request)
        with connection.makefile("rb") as replies:
            first = replies.readline()
            second = replies.readline()

    if (first, second) != (b":1\r\n", b":0\r\n"):
        raise TestFailure(
            f"RESP2 DEL replies: expected :1 then :0, got {first!r}, {second!r}"
        )
    print("[PASS] RESP2 DEL returns 1 then 0 in pipeline order")

    exist_request = b"*2\r\n$5\r\nEXIST\r\n" + key_field

    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        connection.sendall(exist_request + request + exist_request)
        with connection.makefile("rb") as replies:
            before = replies.readline()
            set_reply = replies.readline()
            after = replies.readline()

    if (before, set_reply, after) != (b":0\r\n", b"+OK\r\n", b":1\r\n"):
        raise TestFailure(
            f"RESP2 EXIST replies: {before!r}, {set_reply!r}, {after!r}"
        )
    print("[PASS] RESP2 EXIST returns 0 then 1 in pipeline order")

def test_resp2_wrong_arity(port):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        with connection.makefile("rb") as replies:
            connection.sendall(b"*1\r\n$3\r\nSET\r\n")
            error = replies.readline()
            if error != b"-ERR wrong number of arguments for SET\r\n":
                raise TestFailure(f"SET without arguments returned {error!r}")

            connection.sendall(b"*1\r\n$4\r\nPING\r\n")
            pong = replies.readline()
            if pong != b"+PONG\r\n":
                raise TestFailure(f"connection failed after bad SET: {pong!r}")

    print("[PASS] wrong SET arity returns Error; connection remains usable")

def test_legacy_text_rejected(port):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        with connection.makefile("rb") as replies:
            connection.sendall(b"GET legacy_probe\r\n")
            error = replies.readline()
            if error != b"-ERR invalid or oversized request\r\n":
                raise TestFailure(f"old text command returned {error!r}")

            connection.sendall(b"*1\r\n$4\r\nPING\r\n")
            pong = replies.readline()
            if pong != b"+PONG\r\n":
                raise TestFailure(f"connection failed after old text: {pong!r}")

    print("[PASS] old client text protocol rejected; connection remains usable")

def test_unknown_resp2_command(port):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        with connection.makefile("rb") as replies:
            connection.sendall(b"*1\r\n$5\r\nBOGUS\r\n")
            error = replies.readline()
            if error != b"-ERR unknown command\r\n":
                raise TestFailure(f"unknown command returned {error!r}")

            connection.sendall(b"*1\r\n$4\r\nPING\r\n")
            pong = replies.readline()
            if pong != b"+PONG\r\n":
                raise TestFailure(f"connection failed after unknown command: {pong!r}")

    print("[PASS] unknown RESP2 command returns Error; connection remains usable")

def test_resp2_save(port, test_dir):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        with connection.makefile("rb") as replies:
            connection.sendall(b"*1\r\n$4\r\nSAVE\r\n")
            save_reply = replies.readline()

            connection.sendall(b"*2\r\n$4\r\nSAVE\r\n$1\r\nx\r\n")
            wrong_arity = replies.readline()

    if save_reply != b"+OK\r\n":
        raise TestFailure(f"RESP2 SAVE returned {save_reply!r}")
    if wrong_arity != b"-ERR wrong number of arguments for SAVE\r\n":
        raise TestFailure(f"RESP2 SAVE wrong arity returned {wrong_arity!r}")
    if not (test_dir / "snapshot.db").is_file():
        raise TestFailure("RESP2 SAVE did not create snapshot.db")

    print("[PASS] RESP2 SAVE reply, arity and Snapshot file")

def test_resp2_nul_rejected(port):
    request = (
        b"*3\r\n"
        b"$3\r\nSET\r\n"
        b"$7\r\nnul_key\r\n"
        b"$3\r\na\x00b\r\n"
    )

    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        with connection.makefile("rb") as replies:
            connection.sendall(request)
            error = replies.readline()

            if error != b"-ERR NUL bytes are not supported\r\n":
                raise TestFailure(f"NUL-containing request returned {error!r}")

            connection.sendall(b"*1\r\n$4\r\nPING\r\n")
            pong = replies.readline()

    if pong != b"+PONG\r\n":
        raise TestFailure(f"connection unusable after NUL error: {pong!r}")
    print("[PASS] NUL-containing Bulk rejected; connection remains usable")

def test_fragmented_resp2_with_crlf(port):
    """请求从 value 的 CRLF 中间断开，服务须等完整后再回复。"""
    def encode(*fields):
        parts = [b"*" + str(len(fields)).encode("ascii") + b"\r\n"]
        for field in fields:
            parts.extend((
                b"$" + str(len(field)).encode("ascii") + b"\r\n",
                field,
                b"\r\n",
            ))
        return b"".join(parts)

    key = ("fragment_" + uuid.uuid4().hex).encode("ascii")
    value = b"first line\r\nsecond line with spaces"
    request = encode(b"SET", key, value)
    split_at = request.index(value) + value.index(b"\r\n") + 1

    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.sendall(request[:split_at])

        connection.settimeout(0.2)
        try:
            early_reply = connection.recv(1)
        except socket.timeout:
            pass
        else:
            raise TestFailure(
                f"server replied before complete request: {early_reply!r}"
            )

        connection.settimeout(3)
        connection.sendall(request[split_at:])
        with connection.makefile("rb") as replies:
            set_reply = replies.readline()

            connection.sendall(encode(b"GET", key))
            header = replies.readline()
            body = replies.read(len(value) + 2)

    expected_header = b"$" + str(len(value)).encode("ascii") + b"\r\n"
    if (set_reply != b"+OK\r\n" or
        header != expected_header or
        body != value + b"\r\n"):
        raise TestFailure(
            f"fragmented RESP2 result: {set_reply!r}, {header!r}, {body!r}"
        )
    print("[PASS] fragmented RESP2 request and CRLF value")

def test_empty_and_malformed_resp2(port):
    key = ("empty_" + uuid.uuid4().hex).encode("ascii")
    key_field = b"$" + str(len(key)).encode("ascii") + b"\r\n" + key + b"\r\n"
    set_empty = b"*3\r\n$3\r\nSET\r\n" + key_field + b"$0\r\n\r\n"
    get_value = b"*2\r\n$3\r\nGET\r\n" + key_field

    with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
        connection.settimeout(3)
        with connection.makefile("rb") as replies:
            connection.sendall(set_empty)
            set_reply = replies.readline()

            connection.sendall(get_value)
            get_header = replies.readline()
            get_body = replies.read(2)

            connection.sendall(b"*1\r\n$X\r\n")
            bad_number = replies.readline()

            # 声明长度 3，实际字段 PING 有 4 字节。
            connection.sendall(b"*1\r\n$3\r\nPING\r\n")
            bad_length = replies.readline()

            connection.sendall(b"*1\r\n$4\r\nPING\r\n")
            pong = replies.readline()

    expected_error = b"-ERR invalid or oversized request\r\n"
    if (set_reply != b"+OK\r\n" or
        get_header != b"$0\r\n" or get_body != b"\r\n" or
        bad_number != expected_error or
        bad_length != expected_error or
        pong != b"+PONG\r\n"):
        raise TestFailure(
            f"empty/malformed RESP2: {set_reply!r}, {get_header!r}, "
            f"{get_body!r}, {bad_number!r}, {bad_length!r}, {pong!r}"
        )
    print("[PASS] empty Bulk and malformed frames; connection remains usable")

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
        test_oversized_bulk_header(port)
        test_resp2_ping(port)
        test_resp2_missing_get(port)
        test_resp2_write_commands(port)
        test_resp2_wrong_arity(port)
        test_legacy_text_rejected(port)
        test_unknown_resp2_command(port)
        test_resp2_save(port, test_dir)
        test_resp2_nul_rejected(port)
        test_fragmented_resp2_with_crlf(port)
        test_empty_and_malformed_resp2(port)
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