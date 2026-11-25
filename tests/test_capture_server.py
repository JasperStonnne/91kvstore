# --listen compatibility diagnostics; active client resume/eviction lives in test_capture_push.py.
"""Controlled event/map source + real TCP client; no sudo, RDMA, or Replica changes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import tempfile
import time

CACHE_BYTES = 8 * 1024 * 1024
EVENT = struct.Struct("=IIQQ4096s")


def frame(*fields):
    fields = [field if isinstance(field, bytes) else str(field).encode() for field in fields]
    return b"*%d\r\n" % len(fields) + b"".join(
        b"$%d\r\n" % len(field) + field + b"\r\n" for field in fields)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait(predicate, description, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError(description)


def parse_command(data):
    """Return one complete RESP command, retaining an incomplete suffix across DATA frames."""
    def line(offset):
        end = data.find(b"\r\n", offset)
        return None if end < 0 else (data[offset:end], end + 2)

    first = line(0)
    if first is None:
        return None
    assert first[0].startswith(b"*")
    count = int(first[0][1:])
    assert 1 <= count <= 16
    fields, offset = [], first[1]
    for _ in range(count):
        header = line(offset)
        if header is None:
            return None
        assert header[0].startswith(b"$")
        size = int(header[0][1:])
        assert 0 <= size <= 4096
        offset = header[1]
        if len(data) < offset + size + 2:
            return None
        assert data[offset + size:offset + size + 2] == b"\r\n"
        fields.append(bytes(data[offset:offset + size]))
        offset += size + 2
    return fields, offset


class Client:
    def __init__(self, port, start, host="127.0.0.1", receive_buffer=None):
        self.sock = socket.socket()
        if receive_buffer:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        self.sock.settimeout(8)
        self.sock.connect((host, port))
        self.buffer = bytearray()
        self.commands = bytearray()
        self.received = self.applied = start
        self.count = 0
        self.values = {}

    def send(self, data, fragmented=False):
        if fragmented:
            for byte in data:
                self.sock.sendall(bytes([byte]))
        else:
            self.sock.sendall(data)

    def begin(self, primary, sync, start, fragmented=False):
        self.send(f"BEGIN {primary} {sync} {start}\r\n".encode(), fragmented)

    def take(self, size):
        while len(self.buffer) < size:
            data = self.sock.recv(65536)
            if not data:
                raise EOFError("collector disconnected")
            self.buffer.extend(data)
        result = bytes(self.buffer[:size])
        del self.buffer[:size]
        return result

    def line(self):
        while True:
            end = self.buffer.find(b"\r\n")
            if end >= 0:
                assert end < 256
                return self.take(end + 2)[:-2]
            assert len(self.buffer) <= 256
            data = self.sock.recv(65536)
            if not data:
                raise EOFError("collector disconnected")
            self.buffer.extend(data)

    def read(self):
        parts = self.line().split()
        if parts[0] == b"DATA":
            assert len(parts) == 3 and int(parts[1]) == self.received, parts
            length = int(parts[2])
            assert 0 < length <= 65536
            data = self.take(length)
            self.received += length
            self.commands.extend(data)
            while (command := parse_command(self.commands)) is not None:
                fields, used = command
                if fields[0] in (b"HSET", b"HMOD", b"SET", b"MOD"):
                    self.values[fields[1]] = fields[2]
                elif fields[0] in (b"HDEL", b"DEL"):
                    self.values.pop(fields[1], None)
                else:
                    raise AssertionError(fields)
                del self.commands[:used]
                self.applied += used
                self.count += 1
            return "DATA", data
        if parts[0] == b"ONLINE":
            assert len(parts) == 2 and int(parts[1]) == self.applied and not self.commands, parts
            return "ONLINE", int(parts[1])
        assert len(parts) == 2 and parts[0] == b"ERROR", parts
        return "ERROR", parts[1].decode()

    def ack(self, fragmented=False):
        self.send(f"APPLIED {self.applied}\r\n".encode(), fragmented)

    def quiet(self):
        assert not self.buffer and not select.select([self.sock], [], [], 0.08)[0], \
            "unexpected DATA/ONLINE/EOF"

    def close(self):
        self.sock.close()


class Fixture:
    def __init__(self, repo, root, start=1000, source_pid=None, binary=None):
        root.mkdir()
        self.root = root
        self.primary = "a" * 40
        self.sync = "b" * 40
        self.generation = 2
        self.start = self.end = start
        self.failures = [0, 0, 0]
        self.baseline = [0, 0, 0]
        self.events = root / "events"
        self.events.touch()
        self.publish()
        self.port = free_port()
        self.log_path = root / "capture.log"
        self.log = self.log_path.open("wb")
        self.process = subprocess.Popen(
            [str(binary or repo / "bin/test_capture_mock"), str(source_pid or os.getpid()),
             str(self.events), "--listen", f"127.0.0.1:{self.port}"],
            stdout=self.log, stderr=self.log)
        self.clients = []
        self.expected_exit = 0
        try:
            wait(lambda: "capture TCP listening" in self.text() or self.process.poll() is not None,
                 "collector did not listen")
            assert self.process.poll() is None, self.text()
        except BaseException:
            self.close(check=False)
            raise

    def text(self):
        return self.log_path.read_text(errors="replace")

    def publish(self):
        fields = [self.generation, self.primary, self.sync, self.start, self.end,
                  *self.failures, *self.baseline]
        temporary = self.root / "state.next"
        temporary.write_text(" ".join(map(str, fields)) + "\n")
        temporary.replace(str(self.events) + ".state")

    def emit(self, commands, gap=0):
        records = []
        self.end += gap
        for command in commands:
            encoded = frame(*command)
            assert 0 < len(encoded) <= 4096
            records.append(EVENT.pack(1, len(encoded), self.end, self.end + len(encoded), encoded))
            self.end += len(encoded)
        self.publish()  # Commit watermark precedes event consumption.
        with self.events.open("ab") as stream:
            stream.write(b"".join(records))

    def client(self, fragmented=False, receive_buffer=None, begin=True, **override):
        client = Client(self.port, self.start, receive_buffer=receive_buffer)
        self.clients.append(client)
        if begin:
            client.begin(override.get("primary", self.primary), override.get("sync", self.sync),
                         override.get("start", self.start), fragmented)
        return client

    def new_boundary(self):
        self.generation += 2
        self.sync = f"{self.generation:040x}"
        self.start = self.end
        self.baseline = self.failures.copy()
        self.publish()

    def consumed(self):
        wait(lambda: f"cache_end={self.end} " in self.text(), "collector stopped consuming events")

    def close(self, check=True):
        for client in self.clients:
            client.close()
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)
            raise
        finally:
            self.log.close()
        if check:
            assert "AddressSanitizer" not in self.text() and "LeakSanitizer" not in self.text() and \
                   "runtime error:" not in self.text(), self.text()
            assert self.process.returncode == self.expected_exit, self.text()


def normal(fixture):
    first = ("HSET", "color", "blue")
    second = ("HMOD", "color", "green")
    fixture.emit([first, second])
    target = fixture.end
    client = fixture.client(begin=False)
    begin = f"BEGIN {fixture.primary} {fixture.sync} {fixture.start}\r\n".encode()
    client.send(begin[:40])
    client.quiet()
    client.send(begin[40:])
    assert client.read() == ("DATA", frame(*first) + frame(*second))
    assert client.count == 2 and client.values[b"color"] == b"green"
    client.quiet()  # DATA sent is not yet APPLIED; no premature ONLINE.
    live = ("HSET", "control-text", b"DATA 0 3\r\nONLINE 9\r\nERROR bogus\r\n")
    fixture.emit([live])
    client.quiet()  # Post-T data must follow ONLINE T.
    ack = f"APPLIED {client.applied}\r\n".encode()
    client.send(ack[:-2])
    client.quiet()
    client.send(ack[-2:])
    assert client.read() == ("ONLINE", target)
    assert client.read() == ("DATA", frame(*live))
    client.ack()
    client.ack()  # Duplicate confirmation must not duplicate delivery.
    client.quiet()
    assert client.count == 3 and client.values[b"control-text"] == live[2]


def zero(fixture):
    client = fixture.client(fragmented=True)
    assert client.read() == ("ONLINE", fixture.start)
    client.ack()
    client.quiet()


def lag(fixture):
    command = ("HSET", "later", "value")
    fixture.end += len(frame(*command))
    fixture.publish()
    client = fixture.client()
    client.quiet()
    encoded = frame(*command)
    with fixture.events.open("ab") as stream:
        stream.write(EVENT.pack(1, len(encoded), fixture.start, fixture.end, encoded))
    assert client.read() == ("DATA", encoded)
    client.ack()
    assert client.read() == ("ONLINE", fixture.end)


def identities(fixture):
    for override in ({"primary": "c" * 40}, {"sync": "d" * 40}, {"start": fixture.start + 1}):
        bad = fixture.client(**override)
        assert bad.read() == ("ERROR", "STALE_BOUNDARY")
    good = fixture.client()
    assert good.read() == ("ONLINE", fixture.start)
    busy = fixture.client(begin=False)
    assert busy.read() == ("ERROR", "BUSY")


def invalid_ack(fixture, kind):
    fixture.emit([("HSET", "key", "value")])
    client = fixture.client()
    assert client.read()[0] == "DATA"
    offset = (1 << 64) if kind == "overflow" else fixture.end + 1 if kind == "ahead" else \
             fixture.start - 1 if kind == "regress" else fixture.start + 1
    client.send(f"APPLIED {offset}\r\n".encode())
    assert client.read() == ("ERROR", "INVALID_APPLIED")


def gap(fixture):
    client = fixture.client()
    assert client.read()[0] == "ONLINE"
    fixture.emit([("HSET", "gap", "value")], gap=7)
    assert client.read() == ("ERROR", "CACHE_GAP")


def loss(fixture, reason):
    client = fixture.client()
    assert client.read()[0] == "ONLINE"
    fixture.failures[reason] += 1
    fixture.end += 33  # Last command was committed, but no event or later write follows.
    fixture.publish()
    assert client.read() == ("ERROR", "CAPTURE_LOSS")
    fixture.new_boundary()
    fresh = fixture.client()
    assert fresh.read() == ("ONLINE", fixture.start)


def tail_timeout(fixture):
    fixture.end += 30
    fixture.publish()
    client = fixture.client()
    started = time.monotonic()
    assert client.read() == ("ERROR", "CAPTURE_LAG")
    assert 4.5 < time.monotonic() - started < 8


def reconnect(fixture):
    client = fixture.client()
    assert client.read()[0] == "ONLINE"
    client.close()
    time.sleep(0.1)
    stale = fixture.client()
    assert stale.read() == ("ERROR", "NEED_FULLSYNC")
    fixture.new_boundary()
    fresh = fixture.client()
    assert fresh.read() == ("ONLINE", fixture.start)


def wrap(fixture):
    client = fixture.client()
    assert client.read()[0] == "ONLINE"
    expected_hash = hashlib.sha256()
    received_hash = hashlib.sha256()
    for batch in range(21):
        commands = [("HSET", f"wrap-{index:06}", f"{index:06}" + "x" * 3994)
                    for index in range(batch * 100, (batch + 1) * 100)]
        for command in commands:
            expected_hash.update(frame(*command))
        fixture.emit(commands)
        while client.received < fixture.end:
            kind, payload = client.read()
            assert kind == "DATA"
            received_hash.update(payload)
            client.ack()
        assert client.applied == fixture.end and not client.commands
    assert fixture.end - fixture.start > CACHE_BYTES and client.count == 2100
    assert expected_hash.digest() == received_hash.digest()
    assert len(client.values) == 2100
    for index in range(2100):
        assert client.values[f"wrap-{index:06}".encode()] == (f"{index:06}" + "x" * 3994).encode()
    print(f"[INFO] wrap 2100 unique commands, SHA-256={received_hash.hexdigest()}", flush=True)
    client.quiet()
    fixture.consumed()


def eviction(fixture):
    fixture.emit([("HSET", "blob", "x" * 4000)] * 2100)
    fixture.consumed()
    client = fixture.client()
    assert client.read() == ("ERROR", "CACHE_EVICTED")


def slow(fixture):
    client = fixture.client(receive_buffer=1024)
    assert client.read()[0] == "ONLINE"
    fixture.emit([("HSET", "blob", "x" * 4000)] * 1100)
    fixture.consumed()
    rows = re.findall(r"cache_end=(\d+) valid=\d sent=(\d+) applied=(\d+)", fixture.text())
    assert any(int(end) == fixture.end and int(sent) < fixture.end and int(applied) == fixture.start
               for end, sent, applied in rows), rows
    fixture.emit([("HSET", "blob", "y" * 4000)] * 1100)
    wait(lambda: "error=CACHE_OVERFLOW" in fixture.text(), "pinned cache did not overflow")
    fixture.consumed()
    assert fixture.process.poll() is None
    fixture.emit([("HSET", "after-failure", "still-capturing")])
    fixture.consumed()  # Failed connection did not stall capture or kill the collector.


def malformed(fixture):
    client = fixture.client(begin=False)
    client.send(b"BEGIN " + b"a" * 300)
    assert client.read() == ("ERROR", "PROTOCOL")


def new_boundary(fixture):
    client = fixture.client()
    assert client.read()[0] == "ONLINE"
    fixture.new_boundary()
    assert client.read() == ("ERROR", "BOUNDARY_CHANGED")
    fresh = fixture.client()
    assert fresh.read() == ("ONLINE", fixture.start)


def unknown(fixture):
    Path(str(fixture.events) + ".state").unlink()
    client = fixture.client()
    assert client.read() == ("ERROR", "NO_BOUNDARY")


def applied_timeout(fixture):
    fixture.emit([("HSET", "unconfirmed", "value")])
    client = fixture.client()
    assert client.read()[0] == "DATA"
    # Complete reception alone is insufficient to enter ONLINE or reclaim data.
    assert client.read() == ("ERROR", "APPLIED_TIMEOUT")


def occupied(fixture):
    result = subprocess.run([str(fixture.process.args[0]), str(os.getpid()), str(fixture.events),
                             "--listen", f"127.0.0.1:{fixture.port}"],
                            capture_output=True, text=True, timeout=3)
    assert result.returncode != 0 and "capture TCP server" in result.stderr, result
    good = fixture.client()
    assert good.read()[0] == "ONLINE"  # Existing owner still serves its original port.


def source_change(fixture, rollback=False):
    client = fixture.client()
    assert client.read()[0] == "ONLINE"
    if rollback:
        fixture.emit([("HSET", "new", "value")])
        assert client.read()[0] == "DATA"
        client.ack()
        fixture.end = fixture.start
    else:
        fixture.primary = "c" * 40
        fixture.generation += 2
    fixture.expected_exit = 1
    fixture.publish()
    assert client.read() == ("ERROR", "SOURCE_CHANGED")
    fixture.process.wait(timeout=3)


def source_exit(repo, root, binary):
    source = subprocess.Popen(["sleep", "60"])
    fixture = None
    try:
        fixture = Fixture(repo, root, source_pid=source.pid, binary=binary)
        client = fixture.client()
        assert client.read()[0] == "ONLINE"
        source.terminate()
        source.wait(timeout=3)
        assert client.read() == ("ERROR", "SOURCE_GONE")
        fixture.process.wait(timeout=3)
    finally:
        if fixture is not None:
            fixture.close()
        if source.poll() is None:
            source.terminate()
            source.wait(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--collector", type=Path, help="alternate sanitizer mock binary")
    args = parser.parse_args()
    repo = args.repo.resolve()
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="91kv-capture-server-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Evidence: {root}", flush=True)
    cases = [("zero", zero), ("zero-origin", zero), ("normal", normal), ("lag", lag), ("identities", identities),
             ("ack-ahead", lambda f: invalid_ack(f, "ahead")),
             ("ack-overflow", lambda f: invalid_ack(f, "overflow")),
             ("ack-middle", lambda f: invalid_ack(f, "middle")), ("gap", gap),
             ("ack-regress", lambda f: invalid_ack(f, "regress")),
             *[(f"loss-{n}", lambda f, n=n: loss(f, n)) for n in range(3)],
             ("tail-timeout", tail_timeout), ("reconnect", reconnect),
             ("wrap", wrap), ("eviction", eviction), ("slow", slow),
             ("malformed", malformed), ("boundary-change", new_boundary),
             ("unknown-boundary", unknown), ("applied-timeout", applied_timeout),
             ("occupied-port", occupied), ("source-identity", source_change),
             ("watermark-rollback", lambda f: source_change(f, rollback=True))]
    results = []
    completed = False
    try:
        for name, test in cases:
            fixture = Fixture(repo, root / name, start=0 if name == "zero-origin" else 1000,
                              binary=args.collector)
            try:
                test(fixture)
            finally:
                fixture.close()
            results.append(name)
            print(f"[PASS] capture TCP {name}", flush=True)
        source_exit(repo, root / "source-exit", args.collector)
        results.append("source-exit")
        print("[PASS] capture TCP source-exit", flush=True)
        completed = True
    finally:
        (root / "result.json").write_text(json.dumps(
            {"passed": completed and len(results) == len(cases) + 1,
             "cases": results}, indent=2) + "\n")
    print(f"[PASS] capture TCP: {len(results)} controlled cases; owned collectors stopped", flush=True)


if __name__ == "__main__":
    main()
