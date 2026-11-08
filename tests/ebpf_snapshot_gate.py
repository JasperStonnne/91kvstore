#!/usr/bin/env python3
"""跨 VM 非空追赶验收辅助器。

在 Primary VM 运行 TCP 闸门，暂停 FULLRESYNC_RDMA 通知以固定可重复的写入窗口。
artifacts/release 文件放行通知，artifacts/stop 文件关闭代理。
快照仍走真实 RDMA，代理只接触 TCP 控制和增量。
"""
import argparse
import json
from pathlib import Path
import time

import test_rdma_replication as rdma


def run(args):
    root = args.artifacts.resolve()
    root.mkdir(exist_ok=False)
    rdma.TEST_HOST = args.primary_host
    gate = rdma.SnapshotGate(args.primary_port, connection_timeout=120)
    try:
        (root / "listening.json").write_text(json.dumps({"port": gate.port}))
        print(f"Snapshot gate listening on {args.primary_host}:{gate.port}", flush=True)
        rdma.require(gate.header_ready.wait(120), "Replica did not reach Snapshot gate")
        offset, size = gate.metadata
        (root / "snapshot.json").write_text(json.dumps({"offset": offset, "size": size}))
        deadline = time.monotonic() + 20
        while not (root / "release").exists():
            gate.check()
            rdma.require(time.monotonic() < deadline, "gate release marker timed out")
            time.sleep(0.05)
        gate.release.set()
        print(f"Released Snapshot offset={offset} bytes={size}", flush=True)

        deadline = time.monotonic() + args.timeout
        while not (root / "stop").exists():
            gate.check()
            rdma.require(time.monotonic() < deadline, "gate stop marker timed out")
            time.sleep(0.1)
        (root / "stats.json").write_text(json.dumps({
            "snapshot_offset": offset, "snapshot_bytes": size,
            "tcp_forwarded_bytes": gate.forwarded_bytes
        }))
    finally:
        gate.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--primary-host", required=True)
    parser.add_argument("--primary-port", required=True, type=int)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--timeout", type=int, default=600)
    run(parser.parse_args())
