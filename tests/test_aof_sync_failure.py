#!/usr/bin/env python3
"""同步失败的应用层契约测试；不模拟真实介质故障或断电。

测试专用程序仅对指定临时 AOF 返回一次 EIO。正式程序用来验证重启。
完整命令仍可能留在文件中；没有成功回复不代表命令一定没有生效。
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
                        default=project / "bin/test_kvstore_sync_fault")
    parser.add_argument("--primary-hybrid", action="store_true",
                        help="test Primary snapshot+AOF path (requires ACTIVE SIW)")
    args = parser.parse_args()
    binary, fault_binary = args.binary.resolve(), args.fault_binary.resolve()
    root = Path(tempfile.mkdtemp(prefix="91kv-aof-sync-failure-"))
    directory = root / "data"
    directory.mkdir()
    port = choose_port()
    config = directory / "server.conf"
    host, aof_name = "127.0.0.1", "commands.aof"
    if args.primary_hybrid:
        import test_rdma_replication as rdma
        host = rdma.discover_rdma_host()
        rdma.TEST_HOST = host
        rdma_port = rdma.free_port()
        while rdma_port == port:
            rdma_port = rdma.free_port()
        config.write_text(f"role=primary\nbind_ip={host}\nservice_port={port}\n"
                          f"rdma_port={rdma_port}\nallocator=pool\n"
                          "persistence_mode=legacy_hybrid\n")
        aof_name = "appendonly.aof"
    else:
        config.write_text("role=standalone\nbind_ip=127.0.0.1\n"
                          f"service_port={port}\nallocator=pool\npersistence_mode=aof\n")
    aof, trigger = directory / aof_name, root / "inject-next-sync"

    def invoke(*fields):
        if not args.primary_hybrid:
            return command(port, *fields)
        client = rdma.Client(port)
        try:
            return client.command(*fields)
        finally:
            client.close()
    env = dict(os.environ, KVS_TEST_AOF_PATH=str(aof),
               KVS_TEST_SYNC_TRIGGER=str(trigger))
    owned = []
    result = {
        "mode": "primary/legacy_hybrid" if args.primary_hybrid else "standalone/aof",
        "injection": "one selected sync function returns EIO before real fsync",
        "normal_binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "fault_binary_sha256": hashlib.sha256(fault_binary.read_bytes()).hexdigest(),
        "port": port, "checks_complete": False,
    }
    print(f"[INFO] evidence retained: {root}", flush=True)
    try:
        process = subprocess.Popen(
            [str(fault_binary), str(config)], cwd=directory, env=env,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        owned.append((process, root / "fault.log"))
        wait_for_server(process, port, host)
        # 未放置触发文件时，必须走真正的同步并正常回复。
        assert invoke(b"HSET", b"base", b"committed") == b"OK"
        assert invoke(b"HGET", b"base") == b"committed"
        snapshot_bytes = None
        if args.primary_hybrid:
            assert invoke(b"SAVE") == b"OK"
            snapshot_bytes = (directory / "snapshot.db").read_bytes()
        committed = aof.read_bytes()
        assert committed == encode_command(b"HSET", b"base", b"committed")
        (root / "committed-aof.backup").write_bytes(committed)
        if snapshot_bytes is not None:
            result["snapshot_checkpoint"] = int.from_bytes(snapshot_bytes[24:32], "big")
            assert result["snapshot_checkpoint"] == len(committed)
        trigger.write_text("inject only the next sync of this temporary AOF\n")

        request = encode_command(b"HMOD", b"base", b"after_sync_error")
        reply = bytearray()
        with socket.create_connection((host, port), timeout=3) as sock:
            sock.settimeout(3)
            sock.sendall(request)
            while True:
                try:
                    chunk = sock.recv(1024)
                except ConnectionResetError:
                    break
                if not chunk:
                    break
                reply.extend(chunk)
        output, _ = process.communicate(timeout=5)
        (root / "fault.log").write_bytes(output)
        after = aof.read_bytes()
        result.update(
            client_reply_hex=bytes(reply).hex(), fault_exit=process.returncode,
            committed_aof_bytes=len(committed), aof_bytes_after_failure=len(after),
            failed_command_fully_present=after == committed + request)
        assert not reply, f"sync failure received a response: {reply!r}"
        assert process.returncode == 1, f"unexpected fault exit: {process.returncode}"
        assert output.count(b"test: injected AOF sync EIO") == 1, output
        assert b"failed to append AOF" in output, output
        assert after == committed + request, "expected full command before failed sync"
        if snapshot_bytes is not None:
            assert (directory / "snapshot.db").read_bytes() == snapshot_bytes

        # 不修剪 AOF，用没有故障包装器的正式程序直接重启。
        normal = subprocess.Popen(
            [str(binary), str(config)], cwd=directory,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        owned.append((normal, root / "restart.log"))
        wait_for_server(normal, port, host)
        value = invoke(b"HGET", b"base")
        result["restart_value"] = value.decode() if value is not None else None
        assert value == b"after_sync_error", value
        assert aof.read_bytes() == after, "recovery changed AOF bytes"
        result["restart_aof_unchanged"] = True
        if snapshot_bytes is not None:
            assert (directory / "snapshot.db").read_bytes() == snapshot_bytes
            result["restart_snapshot_unchanged"] = True
        result["checks_complete"] = True
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
            result["service_port_released"] = probe.connect_ex((host, port)) != 0
        result["passed"] = (result["checks_complete"] and
                            result["owned_processes_stopped"] and
                            result["service_port_released"])
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")

    assert result["passed"], result
    print("[PASS] sync EIO: no success response, explicit exit 1, full command retained",
          flush=True)
    print("[PASS] normal restart recovers complete unacknowledged command; AOF unchanged",
          flush=True)
    print("[INFO] application error injection only; real disk failure/power loss unverified",
          flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"[FAIL] AOF sync failure: {error!r}", file=sys.stderr)
        sys.exit(1)
