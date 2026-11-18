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

from test_aof_restart import encode_command, encode_aof
from test_snapshot_restart import command
from test_standalone import choose_port, wait_for_server


def main():
    project = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=project / "bin/kvstore")
    parser.add_argument("--fault-binary", type=Path,
                        default=project / "bin/test_kvstore_sync_fault")
    parser.add_argument("--checksum", choices=("none", "file"), default="none")
    args = parser.parse_args()
    binary, fault_binary = args.binary.resolve(), args.fault_binary.resolve()
    root = Path(tempfile.mkdtemp(prefix="91kv-aof-sync-failure-"))
    directory = root / "data"
    directory.mkdir()
    port = choose_port()
    config = directory / "server.conf"
    host, aof_name = "127.0.0.1", "commands.aof"
    config.write_text("role=standalone\nbind_ip=127.0.0.1\n"
                      f"service_port={port}\nallocator=pool\npersistence_mode=aof\n"
                      f"aof_checksum={args.checksum}\n")
    aof, trigger = directory / aof_name, root / "inject-next-sync"

    env = dict(os.environ, KVS_TEST_AOF_PATH=str(aof),
               KVS_TEST_SYNC_TRIGGER=str(trigger))
    owned = []
    result = {
        "mode": "standalone/aof", "checksum": args.checksum,
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
        assert command(port, b"HSET", b"base", b"committed") == b"OK"
        assert command(port, b"HGET", b"base") == b"committed"
        committed = aof.read_bytes()
        assert committed == encode_aof(encode_command(b"HSET", b"base", b"committed"), args.checksum)
        (root / "committed-aof.backup").write_bytes(committed)
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
        expected_after = encode_aof(
            encode_command(b"HSET", b"base", b"committed") + request, args.checksum)
        result.update(
            client_reply_hex=bytes(reply).hex(), fault_exit=process.returncode,
            committed_aof_bytes=len(committed), aof_bytes_after_failure=len(after),
            failed_command_fully_present=after == expected_after)
        assert not reply, f"sync failure received a response: {reply!r}"
        assert process.returncode == 1, f"unexpected fault exit: {process.returncode}"
        assert output.count(b"test: injected AOF sync EIO") == 1, output
        assert b"failed to append AOF" in output, output
        assert after == expected_after, "expected full command before failed sync"

        # 不修剪 AOF，用没有故障包装器的正式程序直接重启。
        normal = subprocess.Popen(
            [str(binary), str(config)], cwd=directory,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        owned.append((normal, root / "restart.log"))
        wait_for_server(normal, port, host)
        value = command(port, b"HGET", b"base")
        result["restart_value"] = value.decode() if value is not None else None
        assert value == b"after_sync_error", value
        assert aof.read_bytes() == after, "recovery changed AOF bytes"
        result["restart_aof_unchanged"] = True
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
