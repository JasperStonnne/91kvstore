"""Run the socket and independent eBPF modes against the same real RDMA workload."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import time

import test_rdma_replication as rdma


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
        self.capture_socket = root / "capture.sock"

    def launch(self, directory, config):
        if directory.name == "primary":
            config += f"replication_incremental_mode={self.mode}\n"
            if self.mode == "ebpf":
                config += f"replication_capture_socket={self.capture_socket}\n"
        return super().launch(directory, config)


def run(repo, root, mode, mock_collector):
    fixture = Fixture(root, repo / "bin/kvstore", mode)
    gate = writer = reader = collector = None
    mock_events = root / "events"
    offset = 0

    def emit(command):
        nonlocal offset
        data = rdma.frame(*command)
        end = offset + len(data)
        if mock_collector:
            with mock_events.open("ab") as stream:
                stream.write(struct.pack("=IIQQ4096s", 1, len(data), offset, end, data))
        offset = end

    try:
        primary = fixture.start("primary")
        if mode == "ebpf":
            if mock_collector:
                mock_events.touch()
                args = [str(mock_collector), str(primary.pid), str(mock_events)]
            else:
                args = ["sudo", "-n", str(repo / "bin/kvstore-ebpf-capture"),
                        str(primary.pid), str(repo / "build/ebpf_capture.bpf.o")]
            with (root / "capture.log").open("wb") as log:
                collector = subprocess.Popen([*args, "--socket", str(fixture.capture_socket)],
                                             stdout=log, stderr=log)
            fixture.wait(lambda: fixture.capture_socket.exists() or collector.poll() is not None,
                         "capture socket was not created", timeout=10)
            rdma.require(collector.poll() is None, (root / "capture.log").read_text())

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

        commands = [("HMOD", "color", "green"), ("HSET", "extra", "value")]
        for command in commands:
            assert writer.command(*command) == b"OK"
        writer.sock.sendall(rdma.frame("HMOD", "missing", "ignored"))
        assert writer.file.readline() == b"-ERR key not found\r\n"
        gate.release.set()

        primary_log = root / "primary/server.log"
        if mock_collector and mode == "ebpf":
            fixture.wait(lambda: "start capture catch-up" in primary_log.read_text(),
                         "Primary did not start capture catch-up", timeout=10)
            time.sleep(0.1)
            assert not gate.wire, "Primary sent ONLINE or data while capture lagged"
        for command in commands:
            emit(command)

        fixture.online()
        reader = rdma.Client(fixture.replica_port)
        assert reader.command("HGET", "color") == b"green"
        assert reader.command("HGET", "extra") == b"value"
        expected = b"".join(rdma.frame(*cmd) for cmd in commands) + b"ONLINE 107\r\n"
        assert gate.wire == expected

        live = ("HMOD", "color", "online")
        assert writer.command(*live) == b"OK"
        emit(live)
        fixture.wait(lambda: reader.command("HGET", "color") == b"online",
                     "Replica did not apply the ONLINE command", timeout=10)
        assert gate.wire == expected + rdma.frame(*live)
        rdma.assert_rdma_snapshot(fixture)
        for role in ("primary", "replica"):
            assert not (root / role / "appendonly.aof").exists()
            assert not (root / role / "snapshot.db").exists()
        if mode == "ebpf":
            assert "capture session began at offset=35" in (root / "capture.log").read_text()
            assert "start capture catch-up" in primary_log.read_text()
            assert "start cache catch-up" not in primary_log.read_text()
            if mock_collector:
                collector.terminate()
                collector.wait(timeout=3)
                fixture.wait(lambda: "Replica control connection released" in primary_log.read_text(),
                             "collector exit did not disconnect Replica", timeout=10)
                assert writer.command("PING") == b"PONG"
        print(f"[PASS] {mode}: boundary 35, exact catch-up bytes, ONLINE 107 and exact live bytes",
              flush=True)
    finally:
        for client in (reader, writer):
            if client is not None:
                client.close()
        try:
            if gate is not None:
                gate.close()
        finally:
            fixture.close()
            if collector is not None:
                collector.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path)
    parser.add_argument("--mock-collector", type=Path,
                        help="collector linked to a file-backed mock BPF event source")
    args = parser.parse_args()
    rdma.TEST_HOST = rdma.discover_rdma_host()
    with tempfile.TemporaryDirectory(prefix="91kv-capture-replication-") as directory:
        for mode in ("socket", "ebpf"):
            root = Path(directory) / mode
            root.mkdir()
            run(args.repo.resolve(), root, mode, args.mock_collector)


if __name__ == "__main__":
    main()
