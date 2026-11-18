#!/usr/bin/env python3
"""Developer regression for the C manual client; this script owns its test servers.

Not the presentation entry point: use bin/testcase for manual demonstrations.
"""
import argparse
import math
import socket
import threading
import time
from pathlib import Path
import subprocess
import tempfile

from test_snapshot_restart import command, launch
from test_standalone import PROJECT_DIR, choose_port, stop_server, wait_for_server


def client(port, operation, *args, expected=0):
    result = subprocess.run(
        [str(PROJECT_DIR / "bin/testcase"), "127.0.0.1", str(port), operation,
         *map(str, args)], capture_output=True, text=True, timeout=600)
    if result.returncode != expected:
        raise AssertionError(f"exit={result.returncode}, expected={expected}\n"
                             f"{result.stdout}\n{result.stderr}")
    if expected != 2:
        fields = statistics(result.stdout)
        assert fields["op"] == operation, result.stdout
        if expected == 0:
            count = 1 if operation in ("ping", "save") else int(args[0])
            assert fields["attempted"] == fields["passed"] == count, result.stdout
    return result.stdout


def statistics(output):
    lines = [line for line in output.splitlines() if line.startswith(("PASS op=", "FAIL op="))]
    assert len(lines) == 1, output
    parts = lines[0].split()
    fields = dict(part.split("=", 1) for part in parts[1:])
    for name in ("expected", "attempted", "passed", "missing", "mismatched", "errors"):
        fields[name] = int(fields[name])
        assert fields[name] >= 0, output
    seconds, qps = float(fields["seconds"]), float(fields["qps"])
    assert math.isfinite(seconds) and seconds >= 0, output
    assert math.isfinite(qps) and qps >= 0, output
    assert fields["attempted"] == sum(fields[name] for name in
                                       ("passed", "missing", "mismatched", "errors")), output
    assert fields["attempted"] <= fields["expected"], output
    if seconds > 0:
        # 输出的 seconds 和 qps 分别按 9 位、3 位小数舍入。
        assert math.isclose(qps, fields["passed"] / seconds,
                            rel_tol=0.0001, abs_tol=0.001), output
    else:
        assert qps == 0, output
    if parts[0] == "PASS":
        assert fields["attempted"] == fields["passed"] == fields["expected"], output
    fields["seconds"], fields["qps"] = seconds, qps
    return fields


def scripted_statistics(operation, replies, count, expected):
    """真实 TCP 对端精确控制第几条失败，核对客户端的提前退出统计。"""
    failures = []
    received = []
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(5)
        port = listener.getsockname()[1]

        def serve():
            try:
                with listener.accept()[0] as connection:
                    connection.settimeout(5)
                    with connection.makefile("rb") as reader:
                        for reply in replies:
                            line = reader.readline()
                            assert line.startswith(b"*") and line.endswith(b"\r\n"), line
                            for _ in range(int(line[1:-2])):
                                length_line = reader.readline()
                                assert length_line.startswith(b"$"), length_line
                                length = int(length_line[1:-2])
                                assert length >= 0
                                assert len(reader.read(length)) == length
                                assert reader.read(2) == b"\r\n"
                            received.append(line)
                            if reply is None:
                                # 延迟发生在命令尝试内，应计入失败运行的耗时。
                                time.sleep(0.02)
                                return
                            connection.sendall(reply)
            except Exception as error:
                failures.append(error)

        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        try:
            output = client(port, operation, count, expected=1)
        finally:
            worker.join(timeout=6)
        assert not worker.is_alive(), "scripted peer did not stop"
        assert not failures, failures
        assert len(received) == len(replies)
        fields = statistics(output)
        for name, value in expected.items():
            assert fields[name] == value, output
        if replies[-1] is None:
            assert fields["seconds"] >= 0.02, output
        print(f"PASS statistics: {output.strip()}")


def early_failure_statistics():
    scripted_statistics("write", [None], 10,
                        {"attempted": 1, "passed": 0, "errors": 1, "qps": 0})
    for final in (None, b"-ERR injected\r\n", b"invalid RESP\r\n"):
        scripted_statistics("write", [b"+OK\r\n", b"+OK\r\n", final], 10,
                            {"attempted": 3, "passed": 2, "errors": 1,
                             "missing": 0, "mismatched": 0})
    scripted_statistics("write", [b"+OK\r\n", b"+wrong\r\n", b"+OK\r\n"], 3,
                        {"attempted": 3, "passed": 2, "mismatched": 1, "errors": 0})
    scripted_statistics("verify", [b"$-1\r\n"] * 3, 3,
                        {"attempted": 3, "passed": 0, "missing": 3, "errors": 0, "qps": 0})


def persistence(mode, count):
    directory = Path(tempfile.mkdtemp(prefix=f"91kv-manual-{mode}-"))
    port = choose_port()
    (directory / "server.conf").write_text(
        f"role=standalone\nbind_ip=127.0.0.1\nservice_port={port}\n"
        f"log_level=error\npersistence_mode={mode}\n")
    process = None
    try:
        process = launch(directory)
        wait_for_server(process, port)
        client(port, "ping")
        first = count // 2
        client(port, "write", first, 0, "persist")
        client(port, "write", count - first, first, "persist")
        # Check the wire contents independently of the C client's verifier.
        value = command(port, b"HGET", b"manual persist hash 00000000")
        assert value is not None and b"\r\n" in value and b"original" in value
        if mode == "snapshot":
            assert not (directory / "snapshot.bin").exists()
            client(port, "save")
            assert (directory / "snapshot.bin").read_bytes()[:8] == b"91KVSNAP"
            assert not (directory / "commands.aof").exists()
        else:
            assert (directory / "commands.aof").stat().st_size > 0
            assert not (directory / "snapshot.bin").exists()
        process.kill()
        assert process.wait(timeout=10) == -9
        process = launch(directory)
        # Large recovery needs more than the small test's readiness deadline.
        from test_persistence_100k import wait_ready
        wait_ready(process, port)
        output = client(port, "verify", count, 0, "persist")
        assert f"passed={count} missing=0 mismatched=0 errors=0" in output
        # Deliberate corruption and deletion must make verification fail.
        assert command(port, b"HMOD", b"manual persist hash 00000000", b"wrong") == b"OK"
        client(port, "verify", 1, 0, "persist", expected=1)
        assert command(port, b"HDEL", b"manual persist hash 00000000") == 1
        output = client(port, "verify", 1, 0, "persist", expected=1)
        assert "missing=1" in output
        assert not (directory / "server.log").read_bytes().strip()
        print(f"PASS {mode}: {count} records, SIGKILL recovery, negative verification; {directory}")
    finally:
        stop_server(process)


def functions():
    directory = Path(tempfile.mkdtemp(prefix="91kv-manual-functions-"))
    port = choose_port()
    (directory / "server.conf").write_text(
        f"role=standalone\nbind_ip=127.0.0.1\nservice_port={port}\n"
        "log_level=error\npersistence_mode=snapshot\n")
    process = None
    try:
        process = launch(directory)
        wait_for_server(process, port)
        for engine in ("hash", "rbtree", "skiplist", "array"):
            count = 1000 if engine == "array" else 10000
            # Cases must not silently prepare data or run another operation.
            client(port, "verify", 1, 0, "functional", engine, expected=1)
            client(port, "modify", 1, 0, "functional", engine, expected=1)
            client(port, "missing", 1, 0, "functional", engine)
            client(port, "absent", 1, 0, "functional", engine)
            for operation in ("write", "verify", "exists", "modify", "verify-modified",
                              "delete", "missing", "absent"):
                client(port, operation, count, 0, "functional", engine)
                if operation == "write":
                    client(port, "verify-modified", 1, 0, "functional", engine, expected=1)
                    client(port, "verify", 1, 0, "functional", engine)
                elif operation == "modify":
                    client(port, "verify", 1, 0, "functional", engine, expected=1)
            print(f"PASS {engine}: eight independent operations, {count} keys each")
        client(port, "write", 1025, 0, "invalid", "array", expected=2)
        client(port, "write", 0, expected=2)
        client(port, "unknown", 10, expected=2)
        assert not (directory / "snapshot.bin").exists()
        assert not (directory / "server.log").read_bytes().strip()
    finally:
        stop_server(process)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--count", type=int, default=100000)
    args = parser.parse_args()
    if not 2 <= args.count <= 10000000:
        parser.error("count must be between 2 and 10000000")
    early_failure_statistics()
    functions()
    for mode in ("snapshot", "aof"):
        persistence(mode, args.count)
