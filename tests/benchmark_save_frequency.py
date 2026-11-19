#!/usr/bin/env python3
"""Measure synchronous SAVE frequency with one persistent connection per run."""
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
        with Client(port) as client:
            began = time.monotonic()
            for index in range(entries):
                key, value = make_record(index)
                reply = client.command(b"HSET", key, value)
                if reply != b"OK":
                    raise RuntimeError(f"HSET failed at index={index}: {reply!r}")
                result["written"] += 1
                if result["written"] % interval == 0 or result["written"] == entries:
                    save_started = time.monotonic()
                    reply = client.command(b"SAVE")
                    if reply != b"OK":
                        raise RuntimeError(f"SAVE failed after {result['written']} writes: {reply!r}")
                    save_seconds = time.monotonic() - save_started
                    result["saves"].append({"after_writes": result["written"],
                        "seconds": save_seconds, "bytes": (directory / "snapshot.bin").stat().st_size})
            result["seconds"] = time.monotonic() - began
        result["write_qps"] = entries / result["seconds"]
        result["save_seconds"] = sum(item["seconds"] for item in result["saves"])
        result["snapshot_bytes_written"] = sum(item["bytes"] for item in result["saves"])
        if len(result["saves"]) != math.ceil(entries / interval):
            raise RuntimeError("unexpected SAVE count")
        snapshot = directory / "snapshot.bin"
        result["snapshot_sha256"] = digest(snapshot)
        # 恢复和逐条校验在测量区间之后执行，原始文件继续保留。
        stop_server(process)
        process = launch()
        wait_ready(process, port)
        began = time.monotonic()
        with Client(port) as client:
            for index in range(entries):
                key, value = make_record(index)
                if client.command(b"HGET", key) != value:
                    raise RuntimeError(f"recovered value differs at index={index}")
                result["verified"] += 1
        result["verify_seconds"] = time.monotonic() - began
        if digest(snapshot) != result["snapshot_sha256"]:
            raise RuntimeError("recovery changed snapshot bytes")
        if (directory / "commands.aof").exists() or list(directory.glob("snapshot.bin.tmp.*")):
            raise RuntimeError("unexpected AOF or temporary snapshot file")
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
                "timing": "generation + serial writes + synchronous SAVE; recovery verification afterward"}
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
                print(json.dumps({k: result[k] for k in
                    ("status", "entries", "save_every", "seconds", "save_seconds", "write_qps", "verified")}), flush=True)
        success = True
    finally:
        (output / "summary.json").write_text(json.dumps({"passed": success, "results": results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
