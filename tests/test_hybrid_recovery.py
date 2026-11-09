#!/usr/bin/env python3
"""Hybrid 缺日志、日志过短及普通检查点错位的拒绝启动回归；无需 SIW/BPF/root。"""
import argparse
import json
import socket
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def snapshot_fixture(offset):
    # 使用现有 v2 格式：magic、版本、has_offset、记录数、AOF offset。
    # 非空快照用于确认已恢复的数据也不能在缺少 AOF 时对外提供。
    key, value = b"base", b"before_save"
    record = struct.pack(">BII", 3, len(key), len(value)) + key + value
    payload = struct.pack(">8sIIQQ", b"91KVSNAP", 2, 1, 1, offset) + record
    return payload + struct.pack(">I", zlib.crc32(payload))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path,
                        default=Path(__file__).resolve().parent.parent / "bin/kvstore")
    args = parser.parse_args()
    binary = args.binary.resolve()
    root = Path(tempfile.mkdtemp(prefix="91kv-hybrid-recovery-"))
    results = []
    print(f"[INFO] evidence retained: {root}", flush=True)
    try:
        # 完整合法的命令前缀：排除因为 RESP 格式损坏而提前失败的情况。
        command = b"*3\r\n$4\r\nHSET\r\n$4\r\nbase\r\n$11\r\nbefore_save\r\n"
        cases = [
            ("missing-zero", 0, None, b"snapshot.db requires appendonly.aof"),
            ("missing-nonzero", 42, None, b"snapshot.db requires appendonly.aof"),
            ("empty-aof", 1, b"", b"exceeds AOF size"),
            ("complete-prefix-too-short", len(command) + 1, command,
             b"exceeds AOF size"),
            # 保持 AOF 完整、快照 CRC 正确，只把检查点放到命令内部。
            # 覆盖这两种普通错位，不宣称能识别所有不配套的快照/AOF。
            ("inside-command-header", 1, command, b"failed to replay AOF"),
            ("inside-command-value", command.index(b"before_save") + 3,
             command, b"failed to replay AOF"),
        ]
        for name, offset, aof_content, diagnostic in cases:
            directory = root / name
            directory.mkdir()
            snapshot = directory / "snapshot.db"
            original = snapshot_fixture(offset)
            snapshot.write_bytes(original)
            aof = directory / "appendonly.aof"
            if aof_content is not None:
                aof.write_bytes(aof_content)
            port, rdma_port = free_port(), free_port()
            while rdma_port == port:
                rdma_port = free_port()
            config = directory / "server.conf"
            config.write_text(
                "role=primary\nbind_ip=127.0.0.1\n"
                f"service_port={port}\nrdma_port={rdma_port}\n"
                "allocator=pool\npersistence_mode=legacy_hybrid\n")
            # subprocess.run 超时会杀掉并等待它亲自创建的子进程。
            completed = subprocess.run(
                [str(binary), str(config)], cwd=directory,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, timeout=5)
            (directory / "server.log").write_bytes(completed.stdout)
            log = completed.stdout
            assert completed.returncode == 1, (
                f"offset={offset}: expected exit 1, got {completed.returncode}")
            assert diagnostic in log, (
                f"offset={offset}: missing explicit diagnostic: {log!r}")
            assert b"listen port :" not in log, "must reject before serving clients"
            if aof_content is None:
                assert not aof.exists(), "must not create empty AOF"
            else:
                assert aof.read_bytes() == aof_content, "must not repair or truncate AOF"
            assert snapshot.read_bytes() == original, "must preserve snapshot evidence"
            results.append({"case": name, "offset": offset, "exit": completed.returncode,
                            "snapshot_unchanged": True, "aof_unchanged": True})
            print(f"[PASS] {name}: checkpoint={offset}; recovery rejected; files preserved",
                  flush=True)
        (root / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    except Exception:
        print(f"[FAIL] evidence retained: {root}", file=sys.stderr)
        raise


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"[FAIL] hybrid recovery: {error}", file=sys.stderr)
        sys.exit(1)
