"""Real RDMA and collector IPC faults with an explicit file-backed BPF substitute."""
import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

import test_capture_replication as capture
from test_standalone import stop_server

FRAME = struct.Struct("=IIQQ4096s")
REQUEST = struct.Struct("=QII")


def wait(predicate, description, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise TimeoutError(description)


def append(path, start, data):
    with path.open("ab") as stream:
        stream.write(FRAME.pack(1, len(data), start, start + len(data), data))


def collector(repo, root, pid):
    path = root / "events"
    path.touch(exist_ok=True)
    address = root / "capture.sock"
    log_path = root / "capture.log"
    with log_path.open("ab") as log:
        process = subprocess.Popen([str(repo / "bin/test_capture_mock"), str(pid), str(path),
                                    "--socket", str(address)], stdout=log, stderr=log)
    try:
        wait(lambda: address.exists() or process.poll() is not None, "collector readiness")
        assert process.poll() is None, log_path.read_text()
    except BaseException:
        stop_server(process)
        raise
    return process, path, address, log_path


def cache_cases(repo, root):
    for name in ("wrap", "gap", "overflow"):
        directory = root / ("cache-" + name)
        directory.mkdir()
        process, events, address, log = collector(repo, directory, os.getpid())
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as peer:
                peer.settimeout(10)
                peer.connect(str(address))
                if name == "wrap":
                    start = 8 * 1024 * 1024 - 2
                    append(events, start, b"abcdef")
                    wait(lambda: events.stat().st_size > 0, "event file")
                    # 收到 BEGIN 后，尚未消费的事件也从同一边界连续入缓存。
                    peer.sendall(REQUEST.pack(start, 0, 1))
                    assert struct.unpack("=i", peer.recv(4))[0] == 0
                    data = b""
                    deadline = time.monotonic() + 10
                    while not data:
                        if time.monotonic() >= deadline:
                            raise TimeoutError("wrapped event not received")
                        peer.sendall(REQUEST.pack(start, 4096, 0))
                        response = peer.recv(4100)
                        length = struct.unpack("=i", response[:4])[0]
                        assert length >= 0
                        data = response[4:]
                    assert data == b"abcdef"
                else:
                    peer.sendall(REQUEST.pack(0, 0, 1))
                    assert struct.unpack("=i", peer.recv(4))[0] == 0
                    if name == "gap":
                        append(events, 1, b"gap")
                    else:
                        for index in range(2049):
                            append(events, index * 4096, b"x" * 4096)
                    wait(lambda: "capture cache invalid" in log.read_text(), name)
                    peer.sendall(REQUEST.pack(0, 4096, 0))
                    assert struct.unpack("=i", peer.recv(4100)[:4])[0] == -1
            print(f"[PASS] collector IPC {name}", flush=True)
        finally:
            stop_server(process)
            assert not address.exists()


def bulk_cache_read(repo, root):
    """64 KiB IPC packet crossing the cache wrap; preserve every byte."""
    directory = root / "cache-bulk"
    directory.mkdir()
    process, events, address, log = collector(repo, directory, os.getpid())
    start = 8 * 1024 * 1024 - 32768
    payload = bytes(range(256)) * 256
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as peer:
            peer.settimeout(10)
            peer.connect(str(address))
            peer.sendall(REQUEST.pack(start, 0, 1))
            assert struct.unpack("=i", peer.recv(4))[0] == 0
            for index in range(0, len(payload), 4096):
                append(events, start + index, payload[index:index + 4096])
            # Leave a distinct tail to verify the next request offset.
            append(events, start + len(payload), b"marker")
            wait(lambda: f"end={start + len(payload) + 6} valid=1" in log.read_text(),
                 "collector consumed full 64 KiB payload and marker")
            peer.sendall(REQUEST.pack(start, 65536, 0))
            response = peer.recv(65540)
            assert struct.unpack("=i", response[:4])[0] == 65536
            assert response[4:] == payload
            peer.sendall(REQUEST.pack(start + 65536, 65536, 0))
            response = peer.recv(65540)
            assert response == struct.pack("=i", 6) + b"marker"
        print("[PASS] collector IPC full 64 KiB packet, cache wrap and next offset", flush=True)
    finally:
        stop_server(process)
        assert not address.exists()
        assert "failure_count oversize=0 ring_full=0 read_error=0" in log.read_text()


def session_fault(repo, root, fault):
    directory = root / fault
    directory.mkdir()
    fixture = capture.Fixture(directory, repo / "bin/kvstore", "ebpf")
    capture_process = reader = writer = None
    try:
        primary = fixture.start("primary")
        capture_process, events, _, log = collector(repo, directory, primary.pid)
        writer = capture.rdma.Client(fixture.primary_port)
        fixture.start("replica", fixture.primary_port)
        fixture.online()
        fields = ("HSET", "fault_probe", "accepted")
        assert writer.command(*fields) == b"OK"
        data = capture.rdma.frame(*fields)
        if fault == "collector-exit":
            stop_server(capture_process)
            capture_process = None
        elif fault == "gap":
            append(events, 1, data)
        # tail-loss 故意不提供最后一条事件，随后仅 PING，不产生后续写入。
        assert writer.command("PING") == b"PONG"
        replica_log = directory / "replica/server.log"
        fixture.wait(lambda: "Replica control connection closed" in replica_log.read_text(),
                     "fault did not disconnect replica", timeout=12)
        reader = capture.rdma.Client(fixture.replica_port)
        reader.sock.sendall(capture.rdma.frame("HGET", "fault_probe"))
        assert reader.file.readline() == b"-LOADING replica synchronization in progress\r\n"
        reader.close()
        reader = None
        primary_log = directory / "primary/server.log"
        fixture.wait(lambda: "Replica control connection released" in primary_log.read_text(),
                     "Primary did not release failed session")
        # 采集进程重启并丢失缓存后，Replica 重新全量，取回已确认但未复制的命令。
        replica = fixture.processes[-1][0]
        fixture.stop(replica)
        stop_server(capture_process)
        capture_process = None
        events.write_bytes(b"")
        capture_process, _, _, _ = collector(repo, directory, primary.pid)
        fixture.start("replica", fixture.primary_port)
        fixture.online()
        reader = capture.rdma.Client(fixture.replica_port)
        assert reader.command("HGET", "fault_probe") == b"accepted"
        capture.rdma.assert_rdma_snapshot(fixture)
        print(f"[PASS] {fault}: bounded disconnect, stale-read rejection and fresh full sync after cache loss", flush=True)
    finally:
        if reader:
            reader.close()
        if writer:
            writer.close()
        try:
            fixture.close()
        finally:
            stop_server(capture_process)


def socket_overflow(repo, root):
    directory = root / "socket-overflow"
    directory.mkdir()
    fixture = capture.Fixture(directory, repo / "bin/kvstore", "socket")
    writer = reader = gate = None
    try:
        fixture.start("primary")
        writer = capture.rdma.Client(fixture.primary_port)
        gate = capture.Gate(fixture.primary_port)
        fixture.start("replica", gate.port)
        assert gate.header_ready.wait(10)
        gate.check()
        value = "x" * 4000
        for index in range(2100):
            assert writer.command("HSET", f"overflow_{index}", value) == b"OK"
        gate.release.set()
        primary_log = directory / "primary/server.log"
        fixture.wait(lambda: "catch-up cache unavailable" in primary_log.read_text() and
                     "Replica control connection released" in primary_log.read_text(),
                     "overflow session not released", timeout=15)
        gate.thread.join(timeout=5)
        assert not gate.thread.is_alive(), "Primary did not close relay upstream"
        gate.check()
        assert not gate.wire, "overflow session delivered increments or ONLINE"
        # 原有暂停代理不主动向下游传 EOF；确认真实上游 EOF 后关闭下游。
        gate.close()
        gate = None
        log = directory / "replica/server.log"
        fixture.wait(lambda: "Replica control connection closed" in log.read_text(),
                     "Replica did not observe relay EOF", timeout=10)
        assert "replication: enter ONLINE" not in log.read_text()
        fixture.stop(fixture.processes[-1][0])
        fixture.start("replica", fixture.primary_port)
        fixture.online()
        reader = capture.rdma.Client(fixture.replica_port)
        for index in range(2100):
            assert reader.command("HGET", f"overflow_{index}") == value.encode()
        capture.rdma.assert_rdma_snapshot(fixture)
        print("[PASS] socket cache overflow: no ONLINE, fresh full sync verifies all 2100 writes", flush=True)
    finally:
        if reader:
            reader.close()
        if writer:
            writer.close()
        try:
            if gate:
                gate.close()
        finally:
            fixture.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path)
    args = parser.parse_args()
    repo = args.repo.resolve()
    capture.rdma.TEST_HOST = capture.rdma.discover_rdma_host()
    root = Path(tempfile.mkdtemp(prefix="91kv-capture-faults-"))
    print(f"Evidence: {root}", flush=True)
    cache_cases(repo, root)
    bulk_cache_read(repo, root)
    socket_overflow(repo, root)
    for fault in ("tail-loss", "gap", "collector-exit"):
        session_fault(repo, root, fault)


if __name__ == "__main__":
    main()
