#!/usr/bin/env python3
"""Developer regression for the C manual client; this script owns its test servers.

Not the presentation entry point: use bin/testcase for manual demonstrations.
"""
import argparse
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
    return result.stdout


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
    functions()
    for mode in ("snapshot", "aof"):
        persistence(mode, args.count)
