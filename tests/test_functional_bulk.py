#!/usr/bin/env python3
"""一万个不同 key 的完整操作链；Array 遵守现有容量，不扩大单测。"""
import json
import tempfile
from pathlib import Path

from test_persistence_100k import Client, require, wait_ready
from test_snapshot_restart import launch
from test_standalone import choose_port, stop_server

CASES = (("Array", b"", 1000), ("RBTree", b"R", 3000),
         ("Hash", b"H", 3000), ("Skiplist", b"S", 3000))


def records():
    for engine, prefix, count in CASES:
        for index in range(count):
            key = f"functional {engine} {index:05d}".encode()
            yield prefix, key, b"before " + key + b"\r\nline", b"after " + key + b"\r\nline"


def main():
    directory = Path(tempfile.mkdtemp(prefix="91kv-functional-10000-"))
    port = choose_port()
    (directory / "server.conf").write_text(
        f"role=standalone\nbind_ip=127.0.0.1\nservice_port={port}\n"
        "log_level=error\npersistence_mode=snapshot\n", encoding="utf-8")
    result = {"success": False, "distinct_keys": 10000, "phases": {},
              "engines": {name: count for name, _, count in CASES}}
    process = None
    try:
        process = launch(directory)
        wait_ready(process, port)
        # 分阶段操作全部记录，确保读取/修改时一万条记录同时存在。
        phases = (
            ("write", b"SET", lambda old, new: (old,), lambda old, new: b"OK"),
            ("read", b"GET", lambda old, new: (), lambda old, new: old),
            ("modify", b"MOD", lambda old, new: (new,), lambda old, new: b"OK"),
            ("read_modified", b"GET", lambda old, new: (), lambda old, new: new),
            ("exists", b"EXIST", lambda old, new: (), lambda old, new: 1),
            ("delete", b"DEL", lambda old, new: (), lambda old, new: 1),
            ("missing_read", b"GET", lambda old, new: (), lambda old, new: None),
            ("missing_exists", b"EXIST", lambda old, new: (), lambda old, new: 0),
        )
        with Client(port) as client:
            for name, command, arguments, expected in phases:
                result["phases"][name] = 0
                for prefix, key, old, new in records():
                    actual = client.command(prefix + command, key, *arguments(old, new))
                    require(actual == expected(old, new),
                            f"{name}: {key!r}: got {actual!r}, expected {expected(old, new)!r}")
                    result["phases"][name] += 1
                require(result["phases"][name] == 10000, f"{name}: incomplete dataset")
                print(f"[PASS] {name}: 10000 distinct keys", flush=True)
        stop_server(process)
        require(not (directory / "server.log").read_bytes().strip(),
                "error-level successful request path produced terminal output")
        result["success"] = True
    except Exception as exc:
        result["error"] = repr(exc)
        print(f"[FAIL] {exc!r}", flush=True)
    finally:
        stop_server(process)
        (directory / "result.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"[INFO] evidence: {directory}", flush=True)
    return 0 if result["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
