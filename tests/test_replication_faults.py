#!/usr/bin/env python3
"""真实 SIW 全量后的 TCP 追赶/ONLINE 断线；不加载 BPF、不修改系统网络。"""
import argparse
import json
import re
import signal
import subprocess
import time
import socket
import sys
import tempfile
import threading
from pathlib import Path
import test_rdma_replication as rdma

MODES = ("partial-catchup", "before-online", "partial-online",
         "wrong-online-offset", "partial-online-command",
         "primary-crash-before-online", "primary-crash-online",
         "replica-crash-before-online", "replica-crash-online")
ONLINE_CASES = ("partial-online-command", "primary-crash-online", "replica-crash-online")
PREFIXES = ("", "R", "H", "S")


class CutGate(rdma.SnapshotGate):
    def __init__(self, port, mode):
        self.mode = mode
        self.limit = None
        self.catchup = b""
        self.online = b""
        self.wire = bytearray()
        self.source = bytearray()
        self.cut = threading.Event()
        super().__init__(port)

    def forward(self, data, downstream):
        rdma.require(self.limit is not None, "cut boundary was not configured")
        if self.mode == "wrong-online-offset":
            self.source.extend(data)
            if len(self.source) < self.limit:
                return
            expected = self.catchup + self.online
            rdma.require(bytes(self.source[:self.limit]) == expected,
                         "unexpected bytes before ONLINE mutation")
            offset = int(self.online.split()[1])
            outgoing = self.catchup + f"ONLINE {offset + 1}\r\n".encode()
        else:
            outgoing = data[:max(0, self.limit - len(self.wire))]
        if outgoing:
            downstream.sendall(outgoing)
            self.wire.extend(outgoing)
            self.forwarded_bytes += len(outgoing)
        if len(self.wire) >= self.limit or self.mode == "wrong-online-offset":
            self.cut.set()
            self.stopped.set()
            if "-crash-" in self.mode:
                # 到达字节边界后保持连接，控制器先 SIGKILL 专属进程，再释放代理。
                return
            # 只关本代理拥有的控制连接；不触碰 RDMA 设备或其他连接。
            for sock in list(self.sockets):
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def configure(self, catchup, fence, live):
        self.catchup = catchup
        self.online = f"ONLINE {fence}\r\n".encode()
        first = len(rdma.frame("MOD", "base", "catchup"))
        second = len(rdma.frame("DEL", "removed"))
        if self.mode == "partial-catchup":
            self.limit = first + second // 2
        elif self.mode in ("before-online", "primary-crash-before-online",
                           "replica-crash-before-online"):
            self.limit = len(catchup)
        elif self.mode == "partial-online":
            self.limit = len(catchup) + 5
        elif self.mode == "wrong-online-offset":
            self.limit = len(catchup) + len(self.online)
        else:
            self.limit = len(catchup) + len(self.online) + len(live[0]) + len(live[1]) // 2


def mutate(writer, value, added):
    commands = []
    for prefix in PREFIXES:
        for fields, expected in (
                ((prefix + "MOD", "base", value), b"OK"),
                ((prefix + "DEL", "removed"), 1),
                ((prefix + "SET", added, value), b"OK")):
            rdma.require(writer.command(*fields) == expected, f"write failed: {fields}")
            commands.append(rdma.frame(*fields))
    return commands


def check_dataset(client, online_changes, empty=False):
    checked = []
    for prefix in PREFIXES:
        pairs = [("base", b"live" if online_changes and prefix == "" else b"catchup"),
                 ("removed", None), ("during", b"catchup"),
                 ("live_added", None)]
        if online_changes and prefix == "":
            pairs[2] = ("during", None)
            pairs[3] = ("live_added", b"live")
        for key, expected in pairs:
            if empty:
                expected = None
            actual = client.command(prefix + "GET", key)
            rdma.require(actual == expected, f"dataset differs: {prefix} {key}: {actual!r}")
            checked.append([prefix or "Array", key,
                            None if actual is None else actual.decode()])
    return checked


def expect_online(fixture):
    """明确建连失败时立即 FAIL，不依靠重试或等满通用超时掩盖。"""
    log = fixture.root / "replica/server.log"
    fixture.wait(lambda: "enter ONLINE" in log.read_text(errors="replace") or
                 "Replica control connection closed" in log.read_text(errors="replace"),
                 "Replica neither ONLINE nor explicitly disconnected", timeout=10)
    content = log.read_text(errors="replace")
    rdma.require("enter ONLINE" in content,
                 "fresh full sync failed explicitly: " + content[-1200:])


def capture_owned_state(fixture, gate, result):
    """仅采集本轮端口和亲自启动的进程，保留清理前的诊断。"""
    ports = {fixture.primary_port, fixture.replica_port, fixture.rdma_port}
    if gate is not None:
        ports.add(gate.port)
    for key in ("control_proxy_port", "restart_proxy_port"):
        if key in result:
            ports.add(result[key])
    pattern = re.compile(r":(?:" + "|".join(map(str, sorted(ports))) + r")(?:\s|$)")
    sockets = subprocess.run(["ss", "-tanp"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, timeout=3)
    processes = []
    for process, log in fixture.processes:
        entry = {"pid": process.pid, "exit": process.poll(), "log": str(log)}
        proc = Path(f"/proc/{process.pid}")
        if process.poll() is None:
            try:
                entry["status"] = [line for line in (proc / "status").read_text().splitlines()
                                   if line.startswith(("Name:", "State:", "Pid:", "PPid:"))]
                entry["start_ticks"] = (proc / "stat").read_text().split()[21]
            except FileNotFoundError:
                entry["exited_during_capture"] = True
        processes.append(entry)
    return {"ports": sorted(ports), "ss_exit": sockets.returncode,
            "sockets": [line for line in sockets.stdout.splitlines() if pattern.search(line)],
            "processes": processes, "monotonic": time.monotonic()}


def case(fixture, mode):
    primary = fixture.start("primary")
    writer = rdma.Client(fixture.primary_port)
    gate = reader = None
    online_case = mode in ONLINE_CASES
    primary_crash = mode.startswith("primary-crash-")
    replica_crash = mode.startswith("replica-crash-")
    result = {"mode": mode, "checks_complete": False, "trace": []}
    def trace(event):
        result["trace"].append({"event": event, "monotonic": time.monotonic()})
    trace("primary-started")
    try:
        for prefix in PREFIXES:
            rdma.require(writer.command(prefix + "SET", "base", "old") == b"OK", "seed failed")
            rdma.require(writer.command(prefix + "SET", "removed", "kept") == b"OK", "seed failed")
        gate = CutGate(fixture.primary_port, mode)
        if gate.port == fixture.replica_port:
            fixture.replica_port = rdma.free_port()
        result["control_proxy_port"] = gate.port
        replica = fixture.start("replica", gate.port)
        trace("initial-replica-started")
        rdma.require(gate.header_ready.wait(10), "Snapshot header not received")
        gate.check()
        old_id = gate.full_sync_id
        start = gate.stream_offset
        commands = mutate(writer, "catchup", "during")
        catchup = b"".join(commands)
        fence = gate.stream_offset + len(catchup)
        live = [rdma.frame("MOD", "base", "live"), rdma.frame("DEL", "during"),
                rdma.frame("SET", "live_added", "live")]
        gate.configure(catchup, fence, live)
        gate.release.set()
        replica_log = fixture.root / "replica/server.log"
        if online_case:
            expect_online(fixture)
            for fields, expected in (
                    (("MOD", "base", "live"), b"OK"), (("DEL", "during"), 1),
                    (("SET", "live_added", "live"), b"OK")):
                rdma.require(writer.command(*fields) == expected, "live write failed")
        rdma.require(gate.cut.wait(10), "byte boundary not reached")
        gate.check()
        trace("byte-boundary-reached")
        wire = bytes(gate.wire)
        expected_wire = catchup + gate.online + b"".join(live)
        if mode == "wrong-online-offset":
            expected_wire = catchup + f"ONLINE {fence + 1}\r\n".encode()
        else:
            expected_wire = expected_wire[:gate.limit]
        rdma.require(wire == expected_wire, "forwarded bytes differ from expected stream")

        applied = commands[:1] if mode == "partial-catchup" else commands[:]
        if online_case:
            applied.append(live[0])
        ranges = []
        offset = start
        for command in applied:
            ranges.append((offset, offset + len(command)))
            offset += len(command)

        def applied_ranges(log):
            return [(int(first), int(last)) for first, last in re.findall(
                r"applied range=\[(\d+),(\d+)\)", log)]

        if primary_crash or replica_crash:
            fixture.wait(lambda: applied_ranges(replica_log.read_text(errors="replace")) == ranges,
                         "Replica did not reach chosen applied-command boundary", timeout=10)
            victim = primary if primary_crash else replica
            victim.kill()
            victim.wait(timeout=5)
            rdma.require(victim.returncode == -signal.SIGKILL, "not a real SIGKILL exit")
            result["killed_role"] = "primary" if primary_crash else "replica"
            result["crash_exit"] = victim.returncode
            trace("owned-process-killed-and-reaped")
            fixture.stop(victim)  # 从 fixture 活进程列表移除已经退出的专属子进程。
            gate.close()  # 释放暂停的代理，传播已经发生的连接终止。
            gate = None
            trace("old-proxy-released")

        if not replica_crash:
            fixture.wait(lambda: "Replica control connection closed" in
                         replica_log.read_text(errors="replace"),
                         "Replica did not mark disconnection", timeout=10)
        before_log = replica_log.read_text(errors="replace")
        if not online_case:
            rdma.require("enter ONLINE" not in before_log, "false ONLINE before fence")
        else:
            rdma.require(f"enter ONLINE offset={fence}" in before_log, "missing real ONLINE fence")
        rdma.require(applied_ranges(before_log) == ranges,
                     "Replica applied a partial command, duplicate, or unexpected suffix")
        if replica_crash:
            with socket.socket() as probe:
                probe.settimeout(0.3)
                rdma.require(probe.connect_ex((rdma.TEST_HOST, fixture.replica_port)) != 0,
                             "killed Replica still accepts clients")
            refusal = "Replica process exited"
        else:
            reader = rdma.Client(fixture.replica_port)
            rdma.require(reader.command("PING") == b"PONG", "disconnected Replica not alive")
            reader.sock.sendall(rdma.frame("GET", "base"))
            reply = reader.file.readline()
            rdma.require(reply == b"-LOADING replica synchronization in progress\r\n",
                         f"disconnected Replica served stale data: {reply!r}")
            refusal = reply.decode()
            reader.close()
            reader = None
        result.update(snapshot_offset=start, online_fence=fence,
                      expected_replica_offset=offset, applied_ranges=ranges,
                      wire_hex=wire.hex(), disconnected_read_reply=refusal)

        if not replica_crash:
            fixture.stop(replica)
        (fixture.root / "replica/interrupted.log").write_text(before_log)
        if gate is not None:
            gate.close()
            gate = None
        primary_log = fixture.root / "primary/server.log"
        if primary_crash:
            (fixture.root / "primary/crashed.log").write_bytes(primary_log.read_bytes())
            writer.close()
            primary = fixture.start("primary")
            trace("primary-restarted")
            writer = rdma.Client(fixture.primary_port)
        else:
            fixture.wait(lambda: "Replica control connection released" in
                         primary_log.read_text(errors="replace"),
                         "Primary did not release interrupted session", timeout=10)

        fresh = rdma.SnapshotGate(fixture.primary_port)
        gate = fresh
        if fresh.port == fixture.replica_port:
            fixture.replica_port = rdma.free_port()
        result["restart_proxy_port"] = fresh.port
        fixture.start("replica", fresh.port)
        trace("fresh-replica-started")
        rdma.require(fresh.header_ready.wait(10), "restart full-sync header missing")
        fresh.check()
        rdma.require(fresh.full_sync_id != old_id, "restart reused interrupted full-sync ID")
        fresh.release.set()
        expect_online(fixture)
        trace("fresh-full-sync-online")
        reader = rdma.Client(fixture.replica_port)
        result["recovered_dataset"] = check_dataset(reader, online_case, empty=primary_crash)
        reader.close()
        reader = None
        rdma.assert_rdma_snapshot(fixture)
        rdma.require(not (fixture.root / "replica/snapshot.db").exists(),
                     "fresh full sync created a local persistence Snapshot")
        expected_start = 0 if primary_crash else fence + (sum(map(len, live)) if online_case else 0)
        rdma.require(fresh.stream_offset == expected_start, "fresh snapshot boundary differs")
        for role in ("primary", "replica"):
            rdma.require(not (fixture.root / role / "appendonly.aof").exists(),
                         "pure-memory replication wrote AOF")
        result.update(fresh_full_sync_offset=fresh.stream_offset, checks_complete=True)
    finally:
        try:
            result["before_cleanup"] = capture_owned_state(fixture, gate, result)
        except Exception as diagnostic_error:
            result["diagnostic_error"] = repr(diagnostic_error)
        trace("cleanup-started")
        if reader is not None:
            reader.close()
        writer.close()
        if gate is not None:
            gate.close()
        fixture.close()
        trace("cleanup-finished")
        result["owned_processes_stopped"] = all(p.poll() is not None for p, _ in fixture.processes)
        ports_released = True
        for port in (fixture.primary_port, fixture.replica_port):
            with socket.socket() as probe:
                probe.settimeout(0.2)
                ports_released &= probe.connect_ex((rdma.TEST_HOST, port)) != 0
        result["service_ports_released"] = ports_released
        result["passed"] = result["checks_complete"] and result["owned_processes_stopped"] and ports_released
        (fixture.root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    rdma.require(result["passed"], f"failed case: {result}")
    print(f"[PASS] {mode}: exact TCP bytes and applied ranges, unavailable/stale-read rejection, fresh full sync", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--case", choices=("all",) + MODES, default="all")
    args = parser.parse_args()
    repo = args.repo.resolve()
    binary = args.binary.resolve() if args.binary else repo / "bin/kvstore"
    rdma.require(binary.is_file(), f"build program first: {binary}")
    rdma.TEST_HOST = rdma.discover_rdma_host()
    root = Path(tempfile.mkdtemp(prefix="91kv-replication-faults-"))
    root.chmod(0o755)  # 专用临时证据允许普通用户回看；不改用户已有文件权限。
    print(f"[INFO] evidence retained: {root}", flush=True)
    results = []
    try:
        for mode in MODES if args.case == "all" else (args.case,):
            directory = root / mode
            directory.mkdir()
            fixture = rdma.Fixture(directory, binary)
            fixture.log_level = "debug"
            try:
                results.append(case(fixture, mode))
            finally:
                # case 初始启动或 Client 构造也可能失败，仍要释放本轮资产。
                fixture.close()
    finally:
        (root / "summary.json").write_text(json.dumps({"completed": len(results),
            "cases": results}, indent=2) + "\n")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"[FAIL] replication faults: {error!r}", file=sys.stderr)
        sys.exit(1)
