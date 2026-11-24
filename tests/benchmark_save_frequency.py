#!/usr/bin/env python3
"""Measure asynchronous SAVE frequency, foreground latency and final drain."""
from array import array
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import time

from benchmark_memory import make_record
from test_persistence_100k import Client, wait_ready
from test_standalone import choose_port, stop_server


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def run(server, directory, entries, interval):
    directory.mkdir()
    port = choose_port()
    config = directory / "server.conf"
    config.write_text("role=standalone\nbind_ip=127.0.0.1\n"
                      f"service_port={port}\nlog_level=error\nallocator=pool\n"
                      "persistence_mode=snapshot\nsnapshot_checksum=file\n")
    process = None
    result = {"status": "failed", "entries": entries, "save_every": interval,
              "written": 0, "verified": 0, "saves": [],
              "key_bytes": 32, "value_bytes": 128, "engine": "hash"}

    def launch():
        with (directory / "server.log").open("ab") as log:
            child = subprocess.Popen([str(server), str(config)], cwd=directory,
                                     stdin=subprocess.DEVNULL, stdout=log, stderr=log)
        return child

    try:
        process = launch()
        wait_ready(process, port)
        latencies_ns = array("Q")
        with Client(port) as client:
            initial = dict(item.split("=", 1) for item in client.command(b"SAVEINFO").decode().split())
            began = time.monotonic()
            for index in range(entries):
                key, value = make_record(index)
                request_started = time.perf_counter_ns()
                reply = client.command(b"HSET", key, value)
                latencies_ns.append(time.perf_counter_ns() - request_started)
                if reply != b"OK":
                    raise RuntimeError(f"HSET failed at index={index}: {reply!r}")
                result["written"] += 1
                if result["written"] % interval == 0 or result["written"] == entries:
                    save_started = time.monotonic()
                    reply = client.command(b"SAVE")
                    if reply != b"OK":
                        raise RuntimeError(f"SAVE failed after {result['written']} writes: {reply!r}")
                    result["saves"].append({"after_writes": result["written"],
                        "ack_seconds": time.monotonic() - save_started})
                if result["written"] % 100000 == 0:
                    print(f"[every={interval}] written={result['written']}/{entries}", flush=True)
            writes_finished = time.monotonic()
            result["write_seconds"] = writes_finished - began
            # 写入已结束；只在这里等待最后一次 SAVE（包括 pending 接力）落盘。
            deadline = time.monotonic() + 300
            while True:
                state = dict(item.split("=", 1) for item in client.command(b"SAVEINFO").decode().split())
                if int(state["failed"]) != int(initial["failed"]):
                    raise RuntimeError(f"background SAVE failed: {state}")
                if state["in_progress"] == "0" and state["pending"] == "0":
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError(f"final SAVE drain timed out: {state}")
                time.sleep(.01)
            drained = time.monotonic()
        result["drain_seconds"] = drained - writes_finished
        result["seconds"] = drained - began
        result["total_seconds"] = result["seconds"]
        result["write_qps"] = entries / result["write_seconds"]
        result["end_to_end_qps"] = entries / result["total_seconds"]
        result["save_ack_seconds"] = sum(item["ack_seconds"] for item in result["saves"])
        result["triggered"] = int(state["triggered"]) - int(initial["triggered"])
        result["completed"] = int(state["completed"]) - int(initial["completed"])
        result["failed"] = int(state["failed"]) - int(initial["failed"])
        result["total_fork_ms"] = float(state["total_fork_ms"]) - float(initial["total_fork_ms"])
        result["total_save_ms"] = float(state["total_save_ms"]) - float(initial["total_save_ms"])
        result["final_saveinfo"] = state
        expected_saves = math.ceil(entries / interval)
        if len(result["saves"]) != expected_saves or result["triggered"] != expected_saves:
            raise RuntimeError("unexpected SAVE trigger count")
        if not 1 <= result["completed"] <= result["triggered"]:
            raise RuntimeError(f"unexpected SAVE completion count: {state}")
        # nearest-rank 分位数；HSET 请求组装+send 到完整回复，不含生成 key/value 或 SAVE。
        ordered = sorted(latencies_ns)
        result["hset_latency_ms"] = {
            "count": len(ordered),
            "p50": ordered[math.ceil(entries * .50) - 1] / 1e6,
            "p99": ordered[math.ceil(entries * .99) - 1] / 1e6,
            "max": ordered[-1] / 1e6,
        }
        del ordered, latencies_ns
        snapshot = directory / "snapshot.bin"
        result["snapshot_sha256"] = digest(snapshot)
        # 恢复和逐条校验在测量区间之后执行，原始文件继续保留。
        stop_server(process)
        restart_started = time.monotonic()
        process = launch()
        wait_ready(process, port)
        result["restart_seconds"] = time.monotonic() - restart_started
        began = time.monotonic()
        with Client(port) as client:
            for index in range(entries):
                key, value = make_record(index)
                if client.command(b"HGET", key) != value:
                    raise RuntimeError(f"recovered value differs at index={index}")
                result["verified"] += 1
                if result["verified"] % 100000 == 0:
                    print(f"[every={interval}] verified={result['verified']}/{entries}", flush=True)
        result["verify_seconds"] = time.monotonic() - began
        result["snapshot_sha256_after_restart"] = digest(snapshot)
        result["sha256_unchanged"] = result["snapshot_sha256_after_restart"] == result["snapshot_sha256"]
        if not result["sha256_unchanged"]:
            raise RuntimeError("recovery changed snapshot bytes")
        if (directory / "commands.aof").exists() or list(directory.glob("snapshot.bin.tmp.*")):
            raise RuntimeError("unexpected AOF or temporary snapshot file")
        result["snapshot_bytes"] = snapshot.stat().st_size
        result["status"] = "passed"
    except BaseException as error:
        result["error"] = repr(error)
        raise
    finally:
        stop_server(process)
        result["owned_process_stopped"] = process is None or process.poll() is not None
        (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=root / "bin/kvstore")
    parser.add_argument("--entries", type=int, default=1000000)
    parser.add_argument("--save-every", type=int, nargs="+", default=[1000000, 100000, 10000, 1000])
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1 <= args.entries <= 1000000 or args.rounds < 1 or any(i < 1 for i in args.save_every):
        parser.error("entries must be 1..1000000; rounds and intervals must be positive")
    server = args.server.resolve()
    if not server.is_file() or not os.access(server, os.X_OK):
        parser.error(f"executable server missing: {server}")
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="91kv-save-frequency-"))
    metadata = {"arguments": vars(args) | {"server": str(server), "output": str(output)},
                "binary_sha256": digest(server), "script_sha256": digest(Path(__file__)),
                "timing": {"write_seconds": "key/value generation + serial HSET round trips + SAVE accepted replies; no per-SAVE completion wait",
                    "drain_seconds": "after final SAVE reply until SAVEINFO idle and pending=0",
                    "total_seconds": "write_seconds + drain_seconds; excludes startup/recovery/verification",
                    "write_qps": "entries / write_seconds", "end_to_end_qps": "entries / total_seconds",
                    "hset_latency_ms": "perf_counter_ns around Client.command(HSET): encoding/send/full reply; excludes generation and SAVE; nearest-rank P50/P99",
                    "total_fork_ms": "parent foreground fork time from SAVEINFO",
                    "total_save_ms": "child-reported cumulative snapshot function time; overlaps foreground writes"}}
    (output / "metadata.json").write_text(json.dumps(metadata, default=str, indent=2) + "\n")
    print(f"Evidence: {output}", flush=True)
    results = []
    success = False
    try:
        for round_number in range(1, args.rounds + 1):
            rotation = (round_number - 1) % len(args.save_every)
            intervals = args.save_every[rotation:] + args.save_every[:rotation]
            for index, interval in enumerate(intervals):
                directory = output / f"round-{round_number:02d}-case-{index:02d}-every-{interval}"
                result = run(server, directory, args.entries, interval)
                results.append(result)
                (output / "summary.json").write_text(json.dumps({"passed": False, "results": results}, indent=2) + "\n")
                print(json.dumps({k: result[k] for k in
                    ("status", "entries", "save_every", "write_seconds", "drain_seconds", "total_seconds",
                     "write_qps", "end_to_end_qps", "hset_latency_ms", "triggered", "completed", "failed",
                     "total_fork_ms", "total_save_ms", "restart_seconds", "verify_seconds", "verified", "sha256_unchanged")}), flush=True)
        success = True
    finally:
        (output / "summary.json").write_text(json.dumps({"passed": success, "results": results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
