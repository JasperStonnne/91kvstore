#!/usr/bin/env python3
"""真实大快照并发、COW 接力、空闲回收、SIGTERM/SIGINT 退出等待。"""
import json
import os
from pathlib import Path
import signal
import statistics
import subprocess
import tempfile
import threading
import time

from snapshot_helpers import save_info, snapshot_command
from test_persistence_100k import Client
from test_standalone import choose_port, wait_for_server

REPO = Path(__file__).resolve().parent.parent
COUNT = 30000


def main():
    root = Path(tempfile.mkdtemp(prefix="91kv-background-save-"))
    print(f"Evidence: {root}", flush=True)
    directory = root / "data"
    directory.mkdir()
    port = choose_port()
    config = directory / "server.conf"
    config.write_text(f"role=standalone\nbind_ip=127.0.0.1\nservice_port={port}\n"
                      "allocator=pool\nlog_level=error\npersistence_mode=snapshot\n")
    trigger, marker = root / "arm", root / "paused"
    env = dict(os.environ, KVS_TEST_SNAPSHOT_PATH=str(directory / "snapshot.bin"),
               KVS_TEST_SNAPSHOT_TRIGGER=str(trigger), KVS_TEST_SNAPSHOT_REACHED=str(marker),
               KVS_TEST_SNAPSHOT_FAULT="crash-before-file-sync")
    process = None
    worker = None
    stop = threading.Event()
    latencies, errors, final = [], [], [b"initial"]
    def launch(fault=False):
        binary = REPO / "bin" / ("test_kvstore_snapshot_fault" if fault else "kvstore")
        with (root / "server.log").open("ab") as log:
            return subprocess.Popen([str(binary), str(config)], cwd=directory,
                stdout=log, stderr=log, env=env if fault else None, start_new_session=True)
    def key(i):
        return f"async-{i}".encode()
    def value(i):
        return (f"value-{i}:".encode() + b"x" * 512)
    def wait_marker():
        deadline = time.monotonic() + 10
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(.01)
        assert marker.exists(), "child never reached deterministic pause"
        state = save_info(port)
        assert state["in_progress"] == "1", state
        return int(state["pid"])
    def load_worker():
        try:
            with Client(port) as client:
                client.sock.settimeout(1)
                i = 0
                while not stop.is_set():
                    began = time.monotonic()
                    text = str(i).encode()
                    assert client.command(b"HSET", b"live", text) == b"OK"
                    assert client.command(b"HGET", b"live") == text
                    final[0] = text
                    latencies.append(time.monotonic() - began)
                    i += 1
        except BaseException as error:
            errors.append(repr(error))
    try:
        process = launch(True)
        wait_for_server(process, port)
        with Client(port) as client:
            for i in range(COUNT):
                assert client.command(b"HSET", key(i), value(i)) == b"OK"
            assert client.command(b"HSET", b"live", final[0]) == b"OK"
        worker = threading.Thread(target=load_worker)
        worker.start()
        trigger.touch()
        began = time.monotonic()
        assert snapshot_command(port, b"SAVE") == b"OK"
        ack_ms = (time.monotonic() - began) * 1000
        assert ack_ms < 500, ack_ms
        child = wait_marker()
        inherited = [os.readlink(fd) for fd in Path(f"/proc/{child}/fd").iterdir()]
        assert not any(name.startswith("socket:") for name in inherited), inherited
        # 空闲父进程仍服务另一个持续 SET/GET 连接，子进程确定性暂停。
        deadline = time.monotonic() + 3
        while len(latencies) < 200 and time.monotonic() < deadline:
            time.sleep(.01)
        assert len(latencies) >= 200 and not errors, (len(latencies), errors)
        stop.set()
        worker.join(timeout=3)
        assert not worker.is_alive() and not errors, errors
        for _ in range(10):
            assert snapshot_command(port, b"SAVE") == b"OK"
        state = save_info(port)
        assert state["pending"] == "1" and int(state["pid"]) == child, state
        assert state["triggered"] == "11", state
        children = Path(f"/proc/{process.pid}/task/{process.pid}/children").read_text().split()
        assert children == [str(child)], children
        # 接力只保留一个标记，捕获最后一次 SAVE 后仍在父进程中的最新数据。
        with Client(port) as client:
            assert client.command(b"HSET", b"relay-final", b"latest") == b"OK"
        trigger.unlink()
        os.kill(child, signal.SIGCONT)
        # 不发送 SAVEINFO，验证定时任务在无请求时也回收并接力。
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if not Path(f"/proc/{process.pid}/task/{process.pid}/children").read_text().strip():
                break
            time.sleep(.02)
        else:
            raise AssertionError("idle event loop did not reap background saves")
        state = save_info(port)
        assert state["completed"] == "2" and state["failed"] == "0", state
        assert state["pending"] == "0" and state["in_progress"] == "0", state
        assert float(state["total_save_ms"]) >= float(state["last_save_ms"]) > 0, state
        assert int(state["total_fork_us"]) >= int(state["last_fork_us"]) >= 0, state
        assert max(latencies) < .5, max(latencies)
        stats = {"records": COUNT, "ack_ms": ack_ms, "set_get_pairs": len(latencies),
                 "p50_pair_ms": statistics.median(latencies) * 1000,
                 "p99_pair_ms": sorted(latencies)[int(len(latencies)*.99)] * 1000,
                 "max_pair_ms": max(latencies) * 1000, "saveinfo": state}
        process.terminate()
        assert process.wait(timeout=10) == 0
        process = launch()
        wait_for_server(process, port)
        with Client(port) as client:
            for i in range(COUNT):
                assert client.command(b"HGET", key(i)) == value(i), i
            assert client.command(b"HGET", b"live") == final[0]
            assert client.command(b"HGET", b"relay-final") == b"latest"
        process.terminate()
        assert process.wait(timeout=10) == 0
        print("[PASS] 30000 records: concurrent SET/GET latency, single child, coalesced relay, idle reaping and 100% restart verification", flush=True)
        # 两种退出信号都必须等待正在保存的子进程，并完成 pending 接力。
        for stop_signal in (signal.SIGTERM, signal.SIGINT):
            marker.unlink(missing_ok=True)
            trigger.touch()
            process = launch(True)
            wait_for_server(process, port)
            assert snapshot_command(port, b"SAVE") == b"OK"
            child = wait_marker()
            assert snapshot_command(port, b"SAVE") == b"OK"
            with Client(port) as client:
                assert client.command(b"HSET", b"exit-final", str(stop_signal).encode()) == b"OK"
            process.send_signal(stop_signal)
            time.sleep(.1)
            assert process.poll() is None, "parent exited without waiting for child"
            trigger.unlink()
            os.kill(child, signal.SIGCONT)
            assert process.wait(timeout=15) == 0
            assert not Path(f"/proc/{child}").exists(), "snapshot child leaked"
            assert not list(directory.glob("snapshot.bin.tmp.*"))
            process = launch()
            wait_for_server(process, port)
            with Client(port) as client:
                assert client.command(b"HGET", b"exit-final") == str(stop_signal).encode()
            process.terminate()
            assert process.wait(timeout=10) == 0
        print("[PASS] SIGTERM/SIGINT wait for active save and pending relay; latest state recovered; no temp or child leak", flush=True)
        config.write_text(config.read_text().replace("persistence_mode=snapshot", "persistence_mode=none"))
        process = launch()
        wait_for_server(process, port)
        with Client(port) as client:
            for fields, expected in [((b"SAVE",), "ERR SAVE failed"),
                                     ((b"SAVEINFO", b"extra"), "wrong number of arguments for SAVEINFO")]:
                try:
                    client.command(*fields)
                except AssertionError as error:
                    assert expected in str(error), error
                else:
                    raise AssertionError("expected error response")
        assert save_info(port)["triggered"] == "0"
        process.terminate()
        assert process.wait(timeout=10) == 0
        print("[PASS] child inherits no socket FD; none mode rejects SAVE; SAVEINFO arity checked", flush=True)
        (root / "result.json").write_text(json.dumps(stats, indent=2))
        print(json.dumps(stats), flush=True)
    finally:
        stop.set()
        if worker is not None:
            worker.join(timeout=2)
        if process is not None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)


if __name__ == "__main__":
    main()
