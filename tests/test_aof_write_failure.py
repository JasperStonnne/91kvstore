#!/usr/bin/env python3
"""测试专属 AOF 子进程部分写失败：无假成功、退出、拒绝残缺日志恢复。

只修改子进程 RLIMIT_FSIZE，不填磁盘、不改系统设置，无需 root/BPF。
默认单机模式无需 SIW；--primary-hybrid 使用真实 SIW，并先保存快照。
SIGXFSZ 只在故障子进程中忽略，让程序走写错误分支，而非被信号直接杀死。
"""
import argparse
import hashlib
import json
import resource
import signal
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

from test_aof_restart import encode_command
from test_snapshot_restart import command
from test_standalone import choose_port, wait_for_server


def ignore_size_signal():
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path,
                        default=Path(__file__).resolve().parent.parent / "bin/kvstore")
    parser.add_argument("--primary-hybrid", action="store_true",
                        help="test Primary snapshot+AOF path (requires ACTIVE SIW)")
    args = parser.parse_args()
    binary = args.binary.resolve()
    host = "127.0.0.1"
    root = Path(tempfile.mkdtemp(prefix="91kv-aof-write-failure-"))
    port = choose_port()
    aof_name = "commands.aof"
    if args.primary_hybrid:
        import test_rdma_replication as rdma
        host = rdma.discover_rdma_host()
        rdma.TEST_HOST = host
        rdma_port = rdma.free_port()
        while rdma_port == port:
            rdma_port = rdma.free_port()
        config_text = (f"role=primary\nbind_ip={host}\nservice_port={port}\n"
                       f"rdma_port={rdma_port}\nallocator=pool\n"
                       "persistence_mode=legacy_hybrid\n")
        aof_name = "appendonly.aof"
    else:
        config_text = (
            "role=standalone\nbind_ip=127.0.0.1\n"
            f"service_port={port}\nallocator=pool\npersistence_mode=aof\n")

    def invoke(*fields):
        if not args.primary_hybrid:
            return command(port, *fields)
        client = rdma.Client(port)
        try:
            return client.command(*fields)
        finally:
            client.close()
    owned = []
    parent_limits = resource.getrlimit(resource.RLIMIT_FSIZE)
    result = {"binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "port": port, "mode": "primary/legacy_hybrid" if args.primary_hybrid
              else "standalone/aof", "checks_complete": False}
    print(f"[INFO] evidence retained: {root}", flush=True)

    def launch(directory, inject=False):
        directory.mkdir()
        config = directory / "server.conf"
        config.write_text(config_text)
        # 此脚本单线程；限制和信号设置只作用于它亲自创建的子进程。
        process = subprocess.Popen(
            [str(binary), str(config)], cwd=directory,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            preexec_fn=ignore_size_signal if inject else None)
        owned.append((process, directory / "server.log"))
        return process

    try:
        directory = root / "fault"
        process = launch(directory, inject=True)
        wait_for_server(process, port, host)
        assert invoke(b"HSET", b"base", b"committed") == b"OK"
        assert invoke(b"HGET", b"base") == b"committed"
        snapshot_bytes = None
        if args.primary_hybrid:
            assert invoke(b"SAVE") == b"OK"
            snapshot_bytes = (directory / "snapshot.db").read_bytes()
        aof = directory / aof_name
        committed = aof.read_bytes()
        assert committed == encode_command(b"HSET", b"base", b"committed")
        (root / "committed-aof.backup").write_bytes(committed)
        if snapshot_bytes is not None:
            result["snapshot_checkpoint"] = int.from_bytes(snapshot_bytes[24:32], "big")
            assert result["snapshot_checkpoint"] == len(committed)

        # 只允许下一条命令写出 8 字节，迫使剩余写入返回错误。
        _, hard_limit = resource.prlimit(process.pid, resource.RLIMIT_FSIZE)
        limit = len(committed) + 8
        resource.prlimit(process.pid, resource.RLIMIT_FSIZE, (limit, hard_limit))
        request = encode_command(b"HMOD", b"base", b"uncommitted_change")
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
        (directory / "server.log").write_bytes(output)
        damaged = aof.read_bytes()
        result.update(
            committed_aof_bytes=len(committed), file_size_limit=limit,
            client_reply_hex=bytes(reply).hex(), fault_exit=process.returncode,
            partial_tail_hex=damaged[len(committed):].hex())
        assert not reply, f"failed write received response: {reply!r}"
        assert process.returncode == 1, f"unexpected exit: {process.returncode}"
        assert b"failed to append AOF" in output, output
        assert damaged == committed + request[:8], "unexpected AOF bytes after short write"

        # 新进程没有注入限制，仍应因残缺 AOF 而拒绝启动，且不得自动修文件。
        restarted = subprocess.run(
            [str(binary), str(directory / "server.conf")], cwd=directory,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=5)
        (directory / "restart.log").write_bytes(restarted.stdout)
        result["restart_exit"] = restarted.returncode
        assert restarted.returncode == 1, restarted.stdout
        diagnostic = (b"failed to replay AOF" if args.primary_hybrid
                      else b"failed to replay standalone AOF")
        assert diagnostic in restarted.stdout, restarted.stdout
        assert b"listen port :" not in restarted.stdout, restarted.stdout
        assert aof.read_bytes() == damaged, "restart changed damaged AOF"
        if snapshot_bytes is not None:
            assert (directory / "snapshot.db").read_bytes() == snapshot_bytes

        # 独立对照目录只放故障前的完整日志；不修改故障文件。
        control = root / "committed-only-control"
        control.mkdir()
        (control / aof_name).write_bytes(committed)
        if snapshot_bytes is not None:
            (control / "snapshot.db").write_bytes(snapshot_bytes)
        # launch 拥有目录创建，因此此处直接使用已创建目录的配置启动。
        control_config = control / "server.conf"
        control_config.write_text(config_text)
        clean = subprocess.Popen(
            [str(binary), str(control_config)], cwd=control,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)
        owned.append((clean, control / "server.log"))
        wait_for_server(clean, port, host)
        assert invoke(b"HGET", b"base") == b"committed"
        assert (control / aof_name).read_bytes() == committed
        result["committed_only_control_value"] = "committed"
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
        result["parent_limits_unchanged"] = resource.getrlimit(resource.RLIMIT_FSIZE) == parent_limits
        with socket.socket() as probe:
            probe.settimeout(0.3)
            result["service_port_released"] = probe.connect_ex((host, port)) != 0
        result["passed"] = (result["checks_complete"] and
                            result["owned_processes_stopped"] and
                            result["parent_limits_unchanged"] and
                            result["service_port_released"])
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")

    assert result["owned_processes_stopped"] and result["service_port_released"], result
    assert result["parent_limits_unchanged"], result
    print("[PASS] AOF partial write: no success response, exit 1, committed prefix intact",
          flush=True)
    print("[PASS] damaged AOF rejected unchanged; complete baseline restores committed value",
          flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"[FAIL] AOF write failure: {error!r}", file=sys.stderr)
        sys.exit(1)
