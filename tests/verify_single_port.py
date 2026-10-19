#!/usr/bin/env python3
"""Linux-only acceptance checks; run with the repository path as argument.

Does not modify repository files. Uses a TCP relay in the catch-up case to
hold Snapshot delivery while writing new commands to the Primary.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def frame(*fields):
    fields = [x if isinstance(x, bytes) else str(x).encode() for x in fields]
    return b"*%d\r\n" % len(fields) + b"".join(
        b"$%d\r\n" % len(x) + x + b"\r\n" for x in fields
    )


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.file = self.sock.makefile("rb")

    def close(self):
        self.file.close()
        self.sock.close()

    def reply(self):
        line = self.file.readline()
        require(line.endswith(b"\r\n"), "incomplete RESP response")
        if line.startswith(b"+"):
            return line[1:-2]
        if line.startswith(b"$"):
            length = int(line[1:-2])
            if length == -1:
                return None
            require(length >= 0, "invalid bulk response length")
            value = self.file.read(length)
            require(len(value) == length and self.file.read(2) == b"\r\n",
                    "incomplete bulk response")
            return value
        raise RuntimeError(f"unexpected RESP response: {line!r}")

    def command(self, *fields):
        self.sock.sendall(frame(*fields))
        return self.reply()


class Fixture:
    def __init__(self, root, binary):
        self.root = root
        self.binary = binary
        self.processes = []
        self.logs = []
        self.primary_port = free_port()
        self.replica_port = free_port()
        while self.replica_port == self.primary_port:
            self.replica_port = free_port()

    def start(self, role, upstream=None):
        directory = self.root / role
        directory.mkdir()
        port = self.primary_port if role == "primary" else self.replica_port
        config = (
            f"role={role}\nbind_ip=127.0.0.1\nservice_port={port}\n"
            "log_level=info\npersistence_mode=legacy_hybrid\n"
        )
        if role == "replica":
            config += f"primary_host=127.0.0.1\nprimary_port={upstream}\n"
        path = directory / "server.conf"
        path.write_text(config)
        log_path = directory / "server.log"
        log = log_path.open("wb")
        self.logs.append(log)
        process = subprocess.Popen(
            ["stdbuf", "-oL", str(self.binary), str(path)],
            cwd=directory, stdout=log, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
        )
        self.processes.append((process, log_path))
        self.wait(lambda: f"listen port : {port}" in log_path.read_text(errors="replace"),
                  f"{role} did not start")
        return process

    def wait(self, predicate, description, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for process, log in self.processes:
                require(process.poll() is None,
                        f"server exited ({process.returncode}): {log}")
            if predicate():
                return
            time.sleep(0.05)
        raise RuntimeError(description)

    def online(self):
        log = self.root / "replica" / "server.log"
        self.wait(lambda: "replication: enter ONLINE" in log.read_text(errors="replace"),
                  "Replica did not reach ONLINE")

    def assert_listener(self, process):
        inodes = set()
        for name in os.listdir(f"/proc/{process.pid}/fd"):
            try:
                target = os.readlink(f"/proc/{process.pid}/fd/{name}")
            except FileNotFoundError:
                continue
            if target.startswith("socket:["):
                inodes.add(target[8:-1])
        listeners = []
        for table in ("/proc/net/tcp", "/proc/net/tcp6"):
            for line in Path(table).read_text().splitlines()[1:]:
                fields = line.split()
                if fields[9] in inodes and fields[3] == "0A":
                    listeners.append(fields[1])
        expected = f"0100007F:{self.primary_port:04X}"
        require(listeners == [expected],
                f"unexpected Primary LISTEN sockets: {listeners}; expected {expected}")

    def close(self):
        # Replica first; never kill processes that this fixture did not start.
        for process, _ in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for log in self.logs:
            log.close()


class SnapshotGate:
    """A test-only relay; Primary still owns exactly one listening socket."""
    def __init__(self, primary_port):
        self.primary_port = primary_port
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(30)
        self.port = self.listener.getsockname()[1]
        self.header_ready = threading.Event()
        self.release = threading.Event()
        self.stopped = threading.Event()
        self.errors = []
        self.sockets = [self.listener]
        self.metadata = None
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def line(self, sock):
        result = bytearray()
        while not result.endswith(b"\r\n"):
            data = sock.recv(1)
            require(data, "relay connection closed during handshake")
            result += data
            require(len(result) <= 512, "oversized handshake in relay")
        return bytes(result)

    def run(self):
        try:
            downstream, _ = self.listener.accept()
            downstream.settimeout(30)
            self.sockets.append(downstream)
            upstream = socket.socket()
            self.sockets.append(upstream)
            # Constrain receive buffering so the large Snapshot cannot finish
            # sending while the gate is closed. This is not a server listener.
            upstream.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256 * 1024)
            upstream.settimeout(30)
            upstream.connect(("127.0.0.1", self.primary_port))
            ping = self.line(downstream)
            require(ping == b"PING\r\n", f"unexpected Replica handshake: {ping!r}")
            upstream.sendall(ping)
            pong = self.line(upstream)
            require(pong == b"PONG\r\n", f"unexpected Primary handshake: {pong!r}")
            downstream.sendall(pong)
            psync = self.line(downstream)
            require(psync == b"PSYNC ? -1\r\n", f"unexpected PSYNC: {psync!r}")
            upstream.sendall(psync)
            header = self.line(upstream)
            parts = header.split()
            require(len(parts) == 4 and parts[0] == b"FULLRESYNC",
                    f"unexpected full sync header: {header!r}")
            self.metadata = (int(parts[2]), int(parts[3]))
            downstream.sendall(header)
            self.header_ready.set()
            require(self.release.wait(timeout=30), "Snapshot gate was never released")
            upstream.settimeout(0.5)
            while not self.stopped.is_set():
                try:
                    data = upstream.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                downstream.sendall(data)
        except Exception as exc:
            if not self.stopped.is_set():
                self.errors.append(str(exc))
                self.header_ready.set()

    def check(self):
        require(not self.errors, f"relay failed: {self.errors}")

    def close(self):
        self.stopped.set()
        self.release.set()
        for sock in list(self.sockets):
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()
        self.thread.join(timeout=3)
        require(not self.thread.is_alive(), "relay thread did not stop")


def empty_case(fixture):
    primary = fixture.start("primary")
    fixture.assert_listener(primary)
    fixture.start("replica", fixture.primary_port)
    fixture.online()
    client = Client(fixture.replica_port)
    try:
        require(client.command("HGET", "empty_probe") is None,
                "empty Replica unexpectedly contains probe key")
    finally:
        client.close()
    primary_client = Client(fixture.primary_port)
    try:
        # Test reply ordering on the unified Primary port using one pipeline.
        primary_client.sock.sendall(
            frame("HSET", "pipeline_a", "first") +
            frame("HSET", "pipeline_b", "second") +
            frame("HGET", "pipeline_a") + frame("HGET", "pipeline_b")
        )
        replies = [primary_client.reply() for _ in range(4)]
        require(replies == [b"OK", b"OK", b"first", b"second"],
                f"unexpected pipeline reply order: {replies!r}")
    finally:
        primary_client.close()

    def replicated():
        client = Client(fixture.replica_port)
        try:
            return (client.command("HGET", "pipeline_a") == b"first" and
                    client.command("HGET", "pipeline_b") == b"second")
        finally:
            client.close()

    fixture.wait(replicated, "post-empty-sync pipeline writes did not replicate")
    log = (fixture.root / "replica" / "server.log").read_text(errors="replace")
    require("enter FULL_SYNC" in log and "enter CATCH_UP" in log,
            "missing empty full-sync state transitions")
    print("[PASS] empty full sync, ONLINE increments, Primary pipeline ordering", flush=True)


def catchup_case(fixture):
    primary = fixture.start("primary")
    fixture.assert_listener(primary)
    writer = Client(fixture.primary_port)
    gate = None
    try:
        # 512 keys x 64 KiB = 32 MiB. This is a backpressure fixture,
        # not the later 50k + 50k two-machine acceptance workload.
        payload = b"v" * (64 * 1024)
        for first in range(0, 512, 8):
            writer.sock.sendall(b"".join(
                frame("HSET", f"seed_{i}", payload) for i in range(first, first + 8)
            ))
            require(all(writer.reply() == b"OK" for _ in range(8)), "seeding failed")
        print("[INFO] seeded 32 MiB; holding Snapshot delivery", flush=True)
        gate = SnapshotGate(fixture.primary_port)
        if fixture.replica_port == gate.port:
            fixture.replica_port = free_port()
        fixture.start("replica", gate.port)
        require(gate.header_ready.wait(timeout=30), "FULLRESYNC header not received")
        gate.check()
        require(gate.metadata is not None, "missing Snapshot metadata")
        snapshot_offset, snapshot_size = gate.metadata
        require(snapshot_size >= len(payload) * 512, "Snapshot fixture is unexpectedly small")
        require(writer.command("HSET", "catchup_probe", "written while Snapshot held") == b"OK",
                "write during Snapshot failed")
        for i in range(16):
            require(writer.command("HSET", f"catchup_{i}", f"value_{i}") == b"OK",
                    "catch-up write failed")
        gate.release.set()
        fixture.online()
        gate.check()
        client = Client(fixture.replica_port)
        try:
            require(client.command("HGET", "catchup_probe") == b"written while Snapshot held",
                    "catch-up probe did not replicate")
            for i in range(16):
                require(client.command("HGET", f"catchup_{i}") == f"value_{i}".encode(),
                        f"catch-up key {i} differs")
            for i in range(512):
                require(client.command("HGET", f"seed_{i}") == payload,
                        f"Snapshot key {i} differs")
        finally:
            client.close()
        log = (fixture.root / "primary" / "server.log").read_text(errors="replace")
        ranges = [(int(a), int(b)) for a, b in re.findall(
            r"start AOF catch-up range=\[(\d+),(\d+)\)", log
        )]
        require(any(a == snapshot_offset and b > a for a, b in ranges),
                f"nonempty Snapshot-to-AOF catch-up was not demonstrated: {ranges}")
        print(f"[PASS] nonempty AOF catch-up {ranges}; all 512 Snapshot keys and 17 new keys verified",
              flush=True)
    finally:
        writer.close()
        if gate is not None:
            gate.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path, help="Linux repository path")
    args = parser.parse_args()
    binary = args.repo.resolve() / "bin" / "kvstore"
    require(sys.platform.startswith("linux"), "run this script on Linux")
    require(binary.is_file() and os.access(binary, os.X_OK), f"build the server first: {binary}")
    require(shutil.which("stdbuf"), "stdbuf is required")
    root = Path(tempfile.mkdtemp(prefix="91kv-single-port-acceptance-"))
    success = False
    try:
        for name, case in (("empty", empty_case), ("catchup", catchup_case)):
            case_dir = root / name
            case_dir.mkdir()
            fixture = Fixture(case_dir, binary)
            try:
                case(fixture)
            finally:
                fixture.close()
        success = True
        print("[PASS] acceptance scenarios: 2 passed; owned server processes stopped", flush=True)
    finally:
        if success:
            shutil.rmtree(root)
            print("[PASS] temporary directories removed", flush=True)
        else:
            print(f"[FAIL] logs and data retained: {root}", file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        sys.exit(1)
