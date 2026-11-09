#!/usr/bin/env python3
"""Snapshot-only SAVE 的三个失败窗口：替换前保留旧快照，替换后不保证回滚。

仅测试程序包装一次指定的同步或 rename 调用；无需 root、SIW 或 BPF。
这不是实际磁盘故障、断电或 SIGKILL 测试。
"""
import argparse
import hashlib
import json
import os
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

from test_aof_restart import encode_command
from test_snapshot_restart import command
from test_standalone import choose_port, wait_for_server


def main():
    project = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=project / "bin/kvstore")
    parser.add_argument("--fault-binary", type=Path,
                        default=project / "bin/test_kvstore_snapshot_fault")
    parser.add_argument("--case", choices=("file-sync", "rename", "dir-sync"), default="file-sync")
    args = parser.parse_args()
    binary, fault_binary = args.binary.resolve(), args.fault_binary.resolve()
    root = Path(tempfile.mkdtemp(prefix="91kv-snapshot-save-failure-"))
    directory = root / "data"
    directory.mkdir()
    port = choose_port()
    config = directory / "server.conf"
    config.write_text("role=standalone\nbind_ip=127.0.0.1\n"
                      f"service_port={port}\nallocator=pool\npersistence_mode=snapshot\n")
    snapshot, trigger = directory / "snapshot.bin", root / "inject-next-save-failure"
    env = dict(os.environ, KVS_TEST_SNAPSHOT_PATH=str(snapshot),
               KVS_TEST_SNAPSHOT_TRIGGER=str(trigger),
               KVS_TEST_SNAPSHOT_FAULT=args.case)
    owned = []
    result = {
        "mode": "standalone/snapshot", "port": port, "checks_complete": False,
        "injection": args.case + " returns EIO " +
                     ("after rename" if args.case == "dir-sync" else "before replacement"),
        "normal_binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "fault_binary_sha256": hashlib.sha256(fault_binary.read_bytes()).hexdigest(),
    }
    print(f"[INFO] evidence retained: {root}", flush=True)
    try:
        process = subprocess.Popen(
            [str(fault_binary), str(config)], cwd=directory, env=env,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        owned.append((process, root / "fault.log"))
        wait_for_server(process, port)
        assert command(port, b"HSET", b"base", b"saved_old") == b"OK"
        assert command(port, b"SAVE") == b"OK"
        original = snapshot.read_bytes()
        inode = snapshot.stat().st_ino
        (root / "successful-snapshot.backup").write_bytes(original)

        assert command(port, b"HMOD", b"base", b"unsaved_new") == b"OK"
        trigger.write_text("fail only this snapshot's selected SAVE operation\n")
        with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
            sock.settimeout(3)
            sock.sendall(encode_command(b"SAVE"))
            with sock.makefile("rb") as replies:
                reply = replies.readline()
        result["save_reply"] = reply.decode(errors="replace")
        assert reply == b"-ERR SAVE failed\r\n", reply
        assert process.poll() is None, "SAVE failure must not terminate service"
        memory_value = command(port, b"HGET", b"base")
        assert memory_value == b"unsaved_new", memory_value
        after_save = snapshot.read_bytes()
        replaced = args.case == "dir-sync"
        if replaced:
            # rename 已完成；目录同步报错不能被描述成“旧快照仍保留”。
            assert after_save != original, "directory sync failure must occur after replacement"
            assert snapshot.stat().st_ino != inode, "expected new snapshot inode"
        else:
            assert after_save == original, "pre-rename failure replaced old snapshot"
            assert snapshot.stat().st_ino == inode, "old snapshot inode changed"
        assert not list(directory.glob("snapshot.bin.tmp.*")), "temporary file leaked"
        assert not (directory / "commands.aof").exists()
        assert not (directory / "appendonly.aof").exists()
        result.update(memory_after_failed_save=memory_value.decode(),
                      formal_snapshot_unchanged=after_save == original,
                      formal_snapshot_replaced=replaced, temporary_files_remaining=[])

        process.terminate()
        output, _ = process.communicate(timeout=5)
        (root / "fault.log").write_bytes(output)
        diagnostic = {
            "file-sync": b"test: injected Snapshot temporary-file sync EIO",
            "rename": b"test: injected Snapshot rename EIO",
            "dir-sync": b"test: injected Snapshot parent-directory sync EIO",
        }[args.case]
        assert output.count(diagnostic) == 1, output
        result["fault_process_cleanup_exit"] = process.returncode

        normal = subprocess.Popen(
            [str(binary), str(config)], cwd=directory,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        owned.append((normal, root / "restart.log"))
        wait_for_server(normal, port)
        recovered = command(port, b"HGET", b"base")
        expected = b"unsaved_new" if replaced else b"saved_old"
        assert recovered == expected, recovered
        assert snapshot.read_bytes() == after_save, "restart changed snapshot"
        result.update(restart_value=recovered.decode(), checks_complete=True)
    finally:
        for process, log in owned:
            if process.poll() is None:
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
                            result["owned_processes_stopped"] and
                            result["service_port_released"])
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")

    assert result["passed"], result
    print(f"[PASS] SAVE {args.case} error: error reply, service alive, memory retains new value",
          flush=True)
    if args.case == "dir-sync":
        print("[PASS] SAVE failed after rename: new snapshot retained and recovered; no rollback",
              flush=True)
    else:
        print("[PASS] old snapshot unchanged, temp removed, normal restart restores old SAVE",
              flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"[FAIL] Snapshot SAVE failure: {error!r}", file=sys.stderr)
        sys.exit(1)
