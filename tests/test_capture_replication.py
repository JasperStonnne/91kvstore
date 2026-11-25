"""Real RDMA -> SYNC_FINISHED -> socket or independent capture TCP -> real Replica."""
import argparse
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time

import test_rdma_replication as rdma
from test_capture_server_bpf import Collector


class Gate(rdma.SnapshotGate):
    def __init__(self, primary_port):
        self.wire = bytearray()
        super().__init__(primary_port)

    def forward(self, data, downstream):
        self.wire.extend(data)
        super().forward(data, downstream)


class Fixture(rdma.Fixture):
    def __init__(self, root, binary, mode):
        super().__init__(root, binary)
        self.mode = mode
        self.capture_port = rdma.free_port()
        while self.capture_port in (self.primary_port, self.replica_port, self.rdma_port):
            self.capture_port = rdma.free_port()

    def launch(self, directory, config):
        if directory.name == "primary":
            config += f"replication_incremental_mode={self.mode}\n"
            if self.mode == "ebpf":
                config += "# active push: no replication_capture_port required\n"
        return super().launch(directory, config)


def run(repo, root, mode, mock_collector, fault=None):
    fixture = Fixture(root, repo / "bin/kvstore", mode)
    gate = writer = reader = collector = mock_log = None
    mock_events = root / "events"
    mock_events.touch()
    offset = 0
    emitted = []

    def publish():
        if mock_collector and gate and gate.full_sync_id:
            fields = [2, gate.primary_id.decode(), gate.full_sync_id.decode(),
                      gate.stream_offset, offset, 0, 0, 0, 0, 0, 0]
            temporary = root / "state.next"
            temporary.write_text(" ".join(map(str, fields)) + "\n")
            temporary.replace(str(mock_events) + ".state")

    def ready():
        if not mock_collector or mode != "ebpf": return
        # 发布的事件模拟同一个生产就绪探针；先等 Primary 两条件齐备。
        fixture.wait(lambda: f"endpoint={rdma.TEST_HOST}:{fixture.replica_port}" in
            (root / "primary/server.log").read_text(), "Primary did not publish push readiness", timeout=10)
        with Path(str(mock_events) + ".ready").open("ab") as stream:
            stream.write(struct.pack("=IHH16s40s40sQ", 1, fixture.replica_port, 0,
                rdma.TEST_HOST.encode(), gate.primary_id, gate.full_sync_id, gate.stream_offset))

    def emit(command, captured=True):
        nonlocal offset
        data = rdma.frame(*command)
        start, offset = offset, offset + len(data)
        emitted.append((start, offset))
        publish()  # The kernel watermark is updated even when the final event is lost.
        if mock_collector and captured:
            with mock_events.open("ab") as stream:
                stream.write(struct.pack("=IIQQ4096s", 1, len(data), start, offset, data))

    def stop_collector():
        nonlocal collector
        if mock_collector:
            if collector.poll() is None:
                collector.terminate()
            assert collector.wait(timeout=8) == 0
        else:
            collector.close()
        collector = None

    try:
        primary = fixture.start("primary")
        if mode == "ebpf" and fault != "unavailable":
            if mock_collector:
                mock_log = (root / "capture.log").open("wb")
                collector = subprocess.Popen(
                    [str(mock_collector), str(primary.pid), str(mock_events)],
                    stdout=mock_log, stderr=mock_log)
                fixture.wait(lambda: "capture push client ready" in (root / "capture.log").read_text() or
                             collector.poll() is not None, "capture did not listen", timeout=10)
                assert collector.poll() is None, (root / "capture.log").read_text()
            else:
                collector = Collector(repo, primary.pid, None, root / "capture.log", fixture)

        writer = rdma.Client(fixture.primary_port)
        seed = ("HSET", "color", "blue")
        assert writer.command(*seed) == b"OK"
        emit(seed)
        gate = Gate(fixture.primary_port)
        if gate.port == fixture.replica_port:
            fixture.replica_port = rdma.free_port()
        fixture.start("replica", gate.port)
        assert gate.header_ready.wait(10)
        gate.check()
        assert gate.stream_offset == offset == 35
        assert gate.capture_port == (1 if mode == "ebpf" else 0)
        publish()
        commands = [("HMOD", "color", "green"), ("HSET", "extra", "value")]
        if fault == "bulk":
            commands += [("HSET", f"bulk{i}", "b" * 2000) for i in range(60)]
        for command in commands:
            assert writer.command(*command) == b"OK"
            emit(command, captured=not (mock_collector and mode == "ebpf"))
        initial_target = offset
        writer.sock.sendall(rdma.frame("HMOD", "missing", "ignored"))
        assert writer.file.readline() == b"-ERR key not found\r\n"
        gate.release.set()
        primary_log = root / "primary/server.log"
        replica_log = root / "replica/server.log"
        if mock_collector and mode == "ebpf" and fault != "unavailable":
            ready()
            fixture.wait(lambda: "capture connected" in replica_log.read_text(),
                         "Replica did not connect after full load", timeout=10)
            time.sleep(0.1)
            assert not gate.wire
            assert "enter ONLINE" not in replica_log.read_text(), "ONLINE before capture catches up"
        if mock_collector and mode == "ebpf":
            with mock_events.open("ab") as stream:
                for command, (start, end) in zip(commands, emitted[1:]):
                    data = rdma.frame(*command)
                    stream.write(struct.pack("=IIQQ4096s", 1, len(data), start, end, data))

        if fault == "unavailable":
            fixture.wait(lambda: "capture connection closed" in replica_log.read_text(),
                         "unavailable capture did not invalidate the session", timeout=10)
        else:
            fixture.online()
            reader = rdma.Client(fixture.replica_port)
            assert reader.command("HGET", "color") == b"green"
            assert reader.command("HGET", "extra") == b"value"
            expected = b"".join(rdma.frame(*cmd) for cmd in commands) + f"ONLINE {initial_target}\r\n".encode()
            assert gate.wire == (expected if mode == "socket" else b"")
            live = ("HMOD", "color", "online")
            assert writer.command(*live) == b"OK"
            emit(live)
            fixture.wait(lambda: reader.command("HGET", "color") == b"online",
                         "Replica did not apply the ONLINE command", timeout=10)
            assert gate.wire == (expected + rdma.frame(*live) if mode == "socket" else b"")
            for prefix in ("", "R", "H", "S"):
                key = prefix + "direct_probe"
                for verb, value in (("SET", "first"), ("MOD", "second"), ("DEL", None)):
                    command = (prefix + verb, key) if value is None else (prefix + verb, key, value)
                    assert writer.command(*command) == (1 if value is None else b"OK")
                    emit(command)
                    fixture.wait(lambda: reader.command(prefix + "GET", key) ==
                                 (None if value is None else value.encode()),
                                 "four-engine live mutation did not replicate", timeout=10)
            ranges = [tuple(map(int, pair)) for pair in
                      re.findall(r"applied range=\[(\d+),(\d+)\)", replica_log.read_text())]
            assert ranges == emitted[1:], (ranges, emitted[1:])
            if mode == "ebpf":
                assert f"capture TCP ONLINE offset={initial_target}" in (root / "capture.log").read_text()
                assert "hand off increment to capture" in primary_log.read_text()
                assert "start cache catch-up" not in primary_log.read_text()
                if fault == "control-drop":
                    gate.close()
                    gate = None
                    fixture.wait(lambda: "Replica control connection closed" in replica_log.read_text(),
                                 "control loss did not invalidate the incremental connection", timeout=10)
                elif fault in ("tail-loss", "gap"):
                    assert mock_collector
                    command = ("HMOD", "color", "lost-tail")
                    assert writer.command(*command) == b"OK"
                    emit(command, captured=False)
                    if fault == "gap":
                        data = rdma.frame(*command)
                        with mock_events.open("ab") as stream:
                            stream.write(struct.pack("=IIQQ4096s", 1, len(data), offset-len(data)+1, offset+1, data))
                    fixture.wait(lambda: "capture connection closed" in replica_log.read_text(),
                                 "lost last event was not detected by watermark", timeout=10)
                else:
                    stop_collector()
                    fixture.wait(lambda: "capture connection closed" in replica_log.read_text(),
                                 "collector exit did not invalidate Replica", timeout=10)

        if mode == "ebpf":
            fixture.wait(lambda: "Replica control connection released" in primary_log.read_text(),
                         "control connection was not released", timeout=10)
            def upstream_fds_closed():
                pid = fixture.processes[-1][0].pid
                inodes = set()
                for fd in Path(f"/proc/{pid}/fd").iterdir():
                    try:
                        target = os.readlink(fd)
                    except FileNotFoundError:
                        continue
                    if target.startswith("socket:["):
                        inodes.add(target[8:-1])
                upstream_ports = {fixture.capture_port}
                if gate:
                    upstream_ports.add(gate.port)
                for line in Path("/proc/net/tcp").read_text().splitlines()[1:]:
                    fields = line.split()
                    if fields[9] in inodes and int(fields[2].split(":")[1], 16) in upstream_ports:
                        return False
                return True
            fixture.wait(upstream_fds_closed, "cancelled upstream FD was not closed by its owner", timeout=5)
            probe = rdma.Client(fixture.replica_port)
            try:
                probe.sock.sendall(rdma.frame("HGET", "color"))
                assert probe.file.readline().startswith(b"-LOADING"), "Replica served stale data"
            finally:
                probe.close()
            assert writer.command("PING") == b"PONG"
            # 丢失缓存后不续用旧 BEGIN；重启 Replica 发起新全量，含已确认的末条写入。
            if reader:
                reader.close()
                reader = None
            fixture.stop(fixture.processes[-1][0])
            if gate:
                gate.close()
            if collector:
                stop_collector()
            if mock_log:
                mock_log.close()
                mock_log = None
            mock_events.write_bytes(b"")
            Path(str(mock_events) + ".ready").unlink(missing_ok=True)
            Path(str(mock_events) + ".state").unlink(missing_ok=True)
            if mock_collector:
                mock_log = (root / "capture-restarted.log").open("wb")
                collector = subprocess.Popen(
                    [str(mock_collector), str(primary.pid), str(mock_events)],
                    stdout=mock_log, stderr=mock_log)
                fixture.wait(lambda: "capture push client ready" in (root / "capture-restarted.log").read_text(),
                             "restarted collector did not listen", timeout=10)
            else:
                collector = Collector(repo, primary.pid, None,
                                      root / "capture-restarted.log", fixture)
            gate = Gate(fixture.primary_port)
            fixture.start("replica", gate.port)
            assert gate.header_ready.wait(10)
            gate.check()
            assert gate.stream_offset == offset
            publish()
            gate.release.set()
            ready()
            fixture.online()
            reader = rdma.Client(fixture.replica_port)
            expected_color = b"green" if fault == "unavailable" else (
                b"lost-tail" if fault in ("tail-loss", "gap") else b"online")
            assert reader.command("HGET", "color") == expected_color
            assert reader.command("HGET", "extra") == b"value"
            assert not gate.wire
            print(f"[PASS] ebpf{':' + fault if fault else ''}: cache loss requires a fresh full sync, "
                  "new boundary reaches ONLINE without new writes", flush=True)
        rdma.assert_rdma_snapshot(fixture)
        for role in ("primary", "replica"):
            assert not (root / role / "appendonly.aof").exists()
            assert not (root / role / "snapshot.db").exists()
        assert not list(root.rglob("*.sock"))
        print(f"[PASS] {mode}{':' + fault if fault else ''}: real RDMA, load handshake, "
              "direct/ordinary increment, exact applied ranges, no duplicate delivery or .sock", flush=True)
    finally:
        for client in (reader, writer):
            if client is not None:
                client.close()
        try:
            if gate is not None:
                gate.close()
            if collector is not None:
                stop_collector()
        finally:
            fixture.close()
            if mock_log is not None:
                mock_log.close()


def push_timeout_and_injection(repo, root):
    fixture = Fixture(root, repo / "bin/kvstore", "ebpf")
    writer = reader = connection = None
    try:
        fixture.start("primary")
        writer = rdma.Client(fixture.primary_port)
        assert writer.command("HSET", "timeout_seed", "before") == b"OK"
        fixture.start("replica", fixture.primary_port)
        primary_log = root / "primary/server.log"
        replica_log = root / "replica/server.log"
        pattern = r"capture push endpoint=([^:]+):(\d+) offset=(\d+) primary=([0-9a-f]{40}) sync=([0-9a-f]{40})"
        fixture.wait(lambda: len(re.findall(pattern, primary_log.read_text())) >= 2 and
            replica_log.read_text().count("waiting for capture push") >= 2,
            "capture wait did not automatically request a new full sync", timeout=15)
        events = re.findall(pattern, primary_log.read_text())
        assert events[0][4] != events[-1][4], "full-sync identity reused after timeout"
        host, port, offset, primary, sync = events[-1]
        assert "capture wait timed out" in replica_log.read_text()
        assert "enter ONLINE" not in replica_log.read_text()
        begin = f"BEGIN {primary} {sync} {offset}\r\n".encode()
        rejected = [("127.0.0.2", begin),
            (rdma.TEST_HOST, f"BEGIN {'f' * 40} {sync} {offset}\r\n".encode()),
            (rdma.TEST_HOST, f"BEGIN {primary} {events[0][4]} {offset}\r\n".encode()),
            (rdma.TEST_HOST, f"BEGIN {primary} {sync} {int(offset)+1}\r\n".encode())]
        for source, message in rejected:
            with socket.socket() as probe:
                probe.settimeout(3); probe.bind((source, 0)); probe.connect((host, int(port)))
                probe.sendall(message)
                try: assert probe.recv(1) == b"", "invalid BEGIN accepted"
                except ConnectionResetError: pass
        connection = socket.socket(); connection.settimeout(5)
        connection.bind((rdma.TEST_HOST, 0)); connection.connect((host, int(port)))
        connection.sendall(begin[:6])
        time.sleep(.02)
        connection.sendall(begin[6:20])
        time.sleep(.02)
        connection.sendall(begin[20:])
        with connection.makefile("rb") as stream:
            assert stream.readline() == f"APPLIED {offset}\r\n".encode()
            # A second correctly identified connection cannot replace the live link.
            with socket.socket() as probe:
                probe.settimeout(3); probe.bind((rdma.TEST_HOST, 0)); probe.connect((host, int(port)))
                probe.sendall(begin)
                try: assert probe.recv(1) == b""
                except ConnectionResetError: pass
            connection.sendall(f"ONLINE {offset}\r\n".encode())
            fixture.online()
            reader = rdma.Client(fixture.replica_port)
            assert reader.command("HGET", "timeout_seed") == b"before"
        print("[PASS] push admission: actual source IP, Primary/sync IDs, offset, fragmented BEGIN and unique link; 5s timeout automatically retries full sync with fresh ID and recovers ONLINE", flush=True)
    finally:
        if connection: connection.close()
        if reader: reader.close()
        if writer: writer.close()
        fixture.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path)
    parser.add_argument("--mock-collector", type=Path)
    parser.add_argument("--output", type=Path, help="preserve logs and generated configs")
    args = parser.parse_args()
    rdma.TEST_HOST = rdma.discover_rdma_host()
    def suite(root):
        cases = [("socket", None), ("ebpf", None), ("ebpf", "unavailable"), ("ebpf", "control-drop")]
        if args.mock_collector:
            cases.extend([("ebpf", "tail-loss"), ("ebpf", "gap"), ("ebpf", "bulk")])
        for mode, fault in cases:
            directory = root / (mode + ("-" + fault if fault else ""))
            directory.mkdir(parents=True)
            run(args.repo.resolve(), directory, mode, args.mock_collector, fault)
        directory = root / "push-timeout-injection"
        directory.mkdir()
        push_timeout_and_injection(args.repo.resolve(), directory)
    if args.output:
        suite(args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="91kv-capture-replication-") as directory:
            suite(Path(directory))


if __name__ == "__main__":
    main()
