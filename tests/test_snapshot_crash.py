#!/usr/bin/env python3
"""Snapshot-only 七个确定性崩溃窗口；仅 SIGKILL 专属子进程，不模拟断电。"""
import argparse
import hashlib
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

from test_aof_restart import encode_command
from test_snapshot_restart import command
from test_standalone import choose_port, wait_for_server

POINTS = ("before-save", "during-write", "before-file-sync", "after-file-sync",
          "before-rename", "after-rename", "after-dir-sync")
PREFIXES = (b"", b"R", b"H", b"S")


def check_values(port, changed):
    checked = []
    for prefix in PREFIXES:
        expected = ((b"base", b"new" if changed else b"old"),
                    (b"removed", None if changed else b"kept"),
                    (b"added", b"added" if changed else None))
        for key, value in expected:
            actual = command(port, prefix + b"GET", key)
            assert actual == value, (prefix, key, value, actual)
            checked.append({"engine": prefix.decode() or "Array",
                            "key": key.decode(),
                            "value": None if actual is None else actual.decode()})
    return checked


def run_case(args, root, point):
    case = root / point
    directory = case / "data"
    directory.mkdir(parents=True)
    port = choose_port()
    config = directory / "server.conf"
    config.write_text("role=standalone\nbind_ip=127.0.0.1\n"
                      f"service_port={port}\nallocator=pool\npersistence_mode=snapshot\n")
    snapshot, arm, marker = directory / "snapshot.bin", case / "arm", case / "reached"
    env = dict(os.environ, KVS_TEST_SNAPSHOT_PATH=str(snapshot),
               KVS_TEST_SNAPSHOT_TRIGGER=str(arm),
               KVS_TEST_SNAPSHOT_REACHED=str(marker),
               KVS_TEST_SNAPSHOT_FAULT="crash-" + point)
    owned = []
    fault = sock = None
    result = {"point": point, "port": port, "power_loss_test": False,
              "checks_complete": False}
    try:
        fault = subprocess.Popen(
            [str(args.fault_binary), str(config)], cwd=directory, env=env,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        owned.append((fault, case / "crash.log"))
        wait_for_server(fault, port)
        for prefix in PREFIXES:
            assert command(port, prefix + b"SET", b"base", b"old") == b"OK"
            assert command(port, prefix + b"SET", b"removed", b"kept") == b"OK"
        assert command(port, b"SAVE") == b"OK"
        original = snapshot.read_bytes()
        (case / "old-snapshot.backup").write_bytes(original)
        result["before_save_dataset"] = check_values(port, False)
        for prefix in PREFIXES:
            assert command(port, prefix + b"MOD", b"base", b"new") == b"OK"
            assert command(port, prefix + b"DEL", b"removed") == 1
            assert command(port, prefix + b"SET", b"added", b"added") == b"OK"
        result["before_crash_memory"] = check_values(port, True)

        if point != "before-save":
            arm.write_text("pause only this dedicated SAVE\n")
            sock = socket.create_connection(("127.0.0.1", port), timeout=3)
            sock.settimeout(3)
            sock.sendall(encode_command(b"SAVE"))
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                assert fault.poll() is None, "server exited before expected pause"
                status = Path(f"/proc/{fault.pid}/status").read_text()
                state = next(line for line in status.splitlines() if line.startswith("State:"))
                if marker.exists() and "\tT" in state:
                    break
                time.sleep(0.02)
            else:
                raise AssertionError(f"did not reach {point} SIGSTOP")
            assert marker.read_text() == point, marker.read_text()

        replaced = point in ("after-rename", "after-dir-sync")
        formal = snapshot.read_bytes()
        assert (formal != original) == replaced
        temporary = {p.name: p.read_bytes() for p in directory.glob("snapshot.bin.tmp.*")}
        expected_temps = 0 if replaced or point == "before-save" else 1
        assert len(temporary) == expected_temps, temporary.keys()
        if point == "during-write":
            assert len(next(iter(temporary.values()))) == 16
        elif temporary:
            content = next(iter(temporary.values()))
            assert zlib.crc32(content[:-4]) == int.from_bytes(content[-4:], "big")
        result["formal_snapshot_replaced"] = replaced
        result["temporary_files"] = {name: {"bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()} for name, data in temporary.items()}
        fault.kill()
        output, _ = fault.communicate(timeout=5)
        (case / "crash.log").write_bytes(output)
        assert fault.returncode == -signal.SIGKILL, fault.returncode
        if point != "before-save":
            assert f"test: paused Snapshot at {point}".encode() in output, output
        reply = bytearray()
        if sock is not None:
            while True:
                try:
                    data = sock.recv(1024)
                except ConnectionResetError:
                    break
                if not data:
                    break
                reply.extend(data)
            sock.close()
            sock = None
        assert not reply, reply
        result.update(crash_exit=fault.returncode, save_issued=point != "before-save",
                      save_reply_hex=bytes(reply).hex())

        normal = subprocess.Popen(
            [str(args.binary), str(config)], cwd=directory,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        owned.append((normal, case / "restart.log"))
        wait_for_server(normal, port)
        result["recovered_dataset"] = check_values(port, replaced)
        assert snapshot.read_bytes() == formal
        after_temps = {p.name: p.read_bytes() for p in directory.glob("snapshot.bin.tmp.*")}
        assert after_temps == temporary, "restart changed orphan temporary evidence"
        result.update(orphan_temps_ignored_and_preserved=True, checks_complete=True)
    finally:
        if sock is not None:
            sock.close()
        for process, log in owned:
            if process.poll() is None:
                if process is fault:
                    process.kill()
                else:
                    process.terminate()
            try:
                output, _ = process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                output, _ = process.communicate()
            if not log.exists():
                log.write_bytes(output or b"")
        result["owned_processes_stopped"] = all(p.poll() is not None for p, _ in owned)
        with socket.socket() as probe:
            probe.settimeout(0.3)
            result["service_port_released"] = probe.connect_ex(("127.0.0.1", port)) != 0
        result["passed"] = (result["checks_complete"] and
                            result["owned_processes_stopped"] and result["service_port_released"])
        (case / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert result["passed"], result
    print(f"[PASS] SIGKILL {point}: four-engine dataset, snapshot, temps and cleanup verified",
          flush=True)
    return result


def main():
    project = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=project / "bin/kvstore")
    parser.add_argument("--fault-binary", type=Path,
                        default=project / "bin/test_kvstore_snapshot_fault")
    parser.add_argument("--case", choices=("all",) + POINTS, default="all")
    args = parser.parse_args()
    args.binary, args.fault_binary = args.binary.resolve(), args.fault_binary.resolve()
    root = Path(tempfile.mkdtemp(prefix="91kv-snapshot-crash-"))
    print(f"[INFO] evidence retained: {root}", flush=True)
    results = []
    try:
        for point in POINTS if args.case == "all" else (args.case,):
            results.append(run_case(args, root, point))
    finally:
        (root / "summary.json").write_text(json.dumps({
            "normal_binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            "fault_binary_sha256": hashlib.sha256(args.fault_binary.read_bytes()).hexdigest(),
            "completed": len(results), "cases": results}, indent=2) + "\n")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"[FAIL] Snapshot crash: {error!r}", file=sys.stderr)
        sys.exit(1)
