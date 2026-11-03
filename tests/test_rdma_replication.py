#!/usr/bin/env python3
"""Linux 上的真实 RDMA 全量同步验收（由 make test-rdma 调用）。

Primary 与 Replica 在同一台机器的 iWARP 网卡地址上通信，不模拟 RDMA。
TCP 仍负责握手、控制消息和 AOF 增量；快照内容必须通过 RDMA 发送。
自动寻找 ACTIVE iWARP 网卡；也可用 RDMA_TEST_IP 指定其 IPv4 地址。
设备缺失或同步失败会明确报错，不回退到 TCP 全量或将跳过记为通过。
"""
import argparse
import hashlib
import ipaddress
import json
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


# main 在创建任何连接前设置该地址；所有测试连接共用同一张 iWARP 网卡。
TEST_HOST = None
RDMA_CHUNK_CAPACITY = 1024 * 1024


def command_json(*args):
    """仅查询系统网络信息；不创建或修改 RDMA 设备。"""
    result = subprocess.run(args, check=True, capture_output=True, text=True,
                            timeout=10)
    return json.loads(result.stdout)


def discover_rdma_host():
    """选择可用的 iWARP IPv4，避免误用不能路由到 siw 的回环地址。"""
    for tool in ("rdma", "ip", "ibv_devinfo"):
        require(shutil.which(tool), f"真实 RDMA 测试需要命令: {tool}")

    candidates = set()
    for link in command_json("rdma", "-j", "link", "show"):
        if link.get("state") != "ACTIVE" or not link.get("netdev"):
            continue
        device = link["ifname"]
        info = subprocess.run(
            ["ibv_devinfo", "-d", device], check=True,
            capture_output=True, text=True, timeout=10,
        )
        if "iWARP" not in info.stdout:
            continue
        for interface in command_json("ip", "-j", "-4", "addr", "show",
                                      "dev", link["netdev"]):
            for address in interface.get("addr_info", []):
                if address.get("family") == "inet" and address.get("scope") == "global":
                    candidates.add(address["local"])

    requested = os.environ.get("RDMA_TEST_IP")
    if requested:
        requested = str(ipaddress.IPv4Address(requested))
        require(requested in candidates,
                f"RDMA_TEST_IP={requested} 不属于 ACTIVE iWARP 网卡: {sorted(candidates)}")
        return requested
    require(len(candidates) == 1,
            f"需要一个明确的 ACTIVE iWARP IPv4，发现 {sorted(candidates)}；"
            "多地址时请设置 RDMA_TEST_IP")
    return candidates.pop()


def free_port():
    with socket.socket() as sock:
        sock.bind((TEST_HOST, 0))
        return sock.getsockname()[1]


def frame(*fields):
    fields = [x if isinstance(x, bytes) else str(x).encode() for x in fields]
    return b"*%d\r\n" % len(fields) + b"".join(
        b"$%d\r\n" % len(x) + x + b"\r\n" for x in fields
    )


class Client:
    """最小 RESP 客户端，直接核对服务器返回，不依赖额外 Python 包。"""
    def __init__(self, port):
        self.sock = socket.create_connection((TEST_HOST, port), timeout=10)
        self.file = self.sock.makefile("rb")

    def close(self):
        self.file.close()
        self.sock.close()

    def reply(self):
        line = self.file.readline()
        require(line.endswith(b"\r\n"), "incomplete RESP response")
        if line.startswith(b"+"):
            return line[1:-2]
        if line.startswith(b":"):
            return int(line[1:-2])
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
    """拥有每个场景的服务进程、临时配置与日志，退出时只回收自己的资源。"""
    def __init__(self, root, binary):
        self.root = root
        self.binary = binary
        self.processes = []
        self.logs = []
        # CM 使用独立监听端口；TCP 服务端口仍同时接受客户端及复制控制连接。
        self.primary_port = free_port()
        self.replica_port = free_port()
        while self.replica_port == self.primary_port:
            self.replica_port = free_port()

        self.rdma_port = free_port()
        while self.rdma_port in (self.primary_port, self.replica_port):
            self.rdma_port = free_port()

    def start(self, role, upstream=None):
        directory = self.root / role
        directory.mkdir(exist_ok=True)
        port = self.primary_port if role == "primary" else self.replica_port
        config = (
            f"role={role}\nbind_ip={TEST_HOST}\nservice_port={port}\n"
            "allocator=pool\n"
            "log_level=info\npersistence_mode=legacy_hybrid\n"
        )
        if role == "primary":
            config += f"rdma_port={self.rdma_port}\n"
        if role == "replica":
            config += f"primary_host={TEST_HOST}\nprimary_port={upstream}\n"
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
        # CM 的监听由内核 RDMA 子系统管理；这里检查进程持有的 TCP 服务 socket。
        address_hex = socket.inet_aton(TEST_HOST)[::-1].hex().upper()
        expected = f"{address_hex}:{self.primary_port:04X}"
        require(listeners == [expected],
                f"unexpected Primary LISTEN sockets: {listeners}; expected {expected}")

    def stop(self, process):
        owned = [
            item for item in self.processes
            if item[0] is process
        ]
        require(len(owned) == 1,
                "cannot stop a process not owned by this fixture")

        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

        self.processes.remove(owned[0])

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
    """只代理 TCP 控制和 AOF，不接触 RDMA 快照数据。

    收到 FULLRESYNC_RDMA 时，Primary 已经生成快照并固定边界 O。
    暂停该通知，可确定性地在 O 之后写入增量，再放行真实 RDMA 全量。
    不靠 sleep 或网速猜测同步时机，避免快照很小时测试偶发失效。
    """
    def __init__(self, primary_port):
        self.primary_port = primary_port
        self.listener = socket.socket()
        self.listener.bind((TEST_HOST, 0))
        self.listener.listen(1)
        self.listener.settimeout(30)
        self.port = self.listener.getsockname()[1]
        self.header_ready = threading.Event()
        self.release = threading.Event()
        self.stopped = threading.Event()
        self.errors = []
        self.sockets = [self.listener]
        self.metadata = None
        self.forwarded_bytes = 0
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
            upstream.settimeout(30)
            upstream.connect((TEST_HOST, self.primary_port))
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
            require(len(parts) == 6 and parts[0] == b"FULLRESYNC_RDMA",
                    f"expected RDMA full sync, got: {header!r}")
            require(re.fullmatch(rb"[0-9a-f]{40}", parts[1]) is not None and
                    re.fullmatch(rb"[0-9a-f]{40}", parts[5]) is not None,
                    f"invalid replication/full-sync ID: {header!r}")
            require(0 < int(parts[4]) <= 65535, "invalid RDMA port")
            self.metadata = (int(parts[2]), int(parts[3]))
            require(self.metadata[0] >= 0 and self.metadata[1] > 0,
                    "invalid Snapshot offset or size")
            # 必须先暂停通知；若先发通知，Replica 会绕过 TCP 代理开始 RDMA。
            self.header_ready.set()
            require(self.release.wait(timeout=30), "Snapshot gate was never released")
            downstream.sendall(header)
            upstream.settimeout(0.5)
            while not self.stopped.is_set():
                try:
                    data = upstream.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                # 只统计全量通知之后的 TCP 字节，稍后校验恰好是 AOF + ONLINE。
                self.forwarded_bytes += len(data)
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


def assert_rdma_snapshot(fixture):
    """结合传输日志和落盘文件验证真实 RDMA，而非只看最终 ONLINE 状态。"""
    primary_log = (fixture.root / "primary/server.log").read_text(errors="replace")
    replica_log = (fixture.root / "replica/server.log").read_text(errors="replace")
    require("Primary verified RDMA READY" in primary_log, "missing RDMA READY")
    # Replica 重启时 Primary 日志包含两轮传输，只验证当前这一轮。
    session = primary_log.rsplit("Primary verified RDMA READY", 1)[1]
    sent = [int(n) for n in re.findall(r"Primary submitted Snapshot chunk bytes=(\d+)", session)]
    received = [int(n) for n in re.findall(r"Replica wrote Snapshot chunk bytes=(\d+)", replica_log)]
    acknowledgements = [int(n) for n in re.findall(
        r"Primary received Snapshot ACK bytes=(\d+)", session)]
    source = (fixture.root / "primary/replication.snapshot").read_bytes()
    installed = (fixture.root / "replica/snapshot.db").read_bytes()
    require(sent and sent == received, f"RDMA chunk sequence differs: {sent} / {received}")
    require(all(0 < n <= RDMA_CHUNK_CAPACITY for n in sent), "invalid RDMA chunk size")
    cumulative = []
    total = 0
    for length in sent:
        total += length
        cumulative.append(total)
    require(acknowledgements == cumulative,
            f"cumulative ACK differs: {acknowledgements} / {cumulative}")
    require(total == len(source) == len(installed), "Snapshot byte count differs")
    require(hashlib.sha256(source).digest() == hashlib.sha256(installed).digest(),
            "installed Snapshot differs from Primary Snapshot")
    require(f"RDMA Snapshot fully acknowledged bytes={total}" in session,
            "missing final Snapshot ACK")
    require("Primary RDMA closed, start TCP catch-up" in session and
            "Replica RDMA closed, resume TCP catch-up" in replica_log,
            "RDMA did not close normally before TCP catch-up")
    require(not (fixture.root / "replica/snapshot.db.replication.tmp").exists(),
            "Snapshot temporary file was not committed")
    print(f"[PASS] real RDMA Snapshot: {total} bytes, {len(sent)} chunks, cumulative ACK and SHA-256",
          flush=True)
    return total, sent


def assert_tcp_catchup_only(fixture, gate):
    """全量通知后，TCP 字节数只能等于 [O, ONLINE offset) 的 AOF 加 ONLINE 行。"""
    log = (fixture.root / "replica/server.log").read_text(errors="replace")
    offsets = re.findall(r"enter ONLINE offset=(\d+)", log)
    require(offsets, "missing ONLINE offset")
    online_offset = int(offsets[-1])
    start, _ = gate.metadata
    expected = online_offset - start + len(f"ONLINE {online_offset}\r\n".encode())
    require(gate.forwarded_bytes == expected,
            f"unexpected TCP payload: {gate.forwarded_bytes} bytes; AOF + ONLINE needs {expected}")


def verify_online_mutations(fixture):
    """全量完成后继续插入、修改、删除，验证增量通路没有停在 ONLINE。"""
    writer = Client(fixture.primary_port)
    reader = Client(fixture.replica_port)
    try:
        require(writer.command("SET", "online_probe", "first") == b"OK", "ONLINE SET failed")
        fixture.wait(lambda: reader.command("GET", "online_probe") == b"first",
                     "ONLINE insertion did not replicate")
        require(writer.command("MOD", "online_probe", "second") == b"OK", "ONLINE MOD failed")
        fixture.wait(lambda: reader.command("GET", "online_probe") == b"second",
                     "ONLINE modification did not replicate")
        require(writer.command("DEL", "online_probe") == 1, "ONLINE DEL failed")
        fixture.wait(lambda: reader.command("GET", "online_probe") is None,
                     "ONLINE deletion did not replicate")
    finally:
        reader.close()
        writer.close()
    print("[PASS] ONLINE AOF insertion, modification and deletion", flush=True)


def exact_chunk_case(fixture):
    """恰好 1 MiB 的快照：最终块不能多发，也不能等一个不存在的下一块。"""
    fixture.start("primary")
    writer = Client(fixture.primary_port)
    # v2 头、检查点、单条记录和 CRC 共 53 字节（key 为 8 字节的 boundary）。
    payload = b"z" * (RDMA_CHUNK_CAPACITY - 53)
    try:
        require(writer.command("SET", "boundary", payload) == b"OK", "boundary seed failed")
    finally:
        writer.close()
    fixture.start("replica", fixture.primary_port)
    fixture.online()
    size, chunks = assert_rdma_snapshot(fixture)
    require(size == RDMA_CHUNK_CAPACITY and chunks == [RDMA_CHUNK_CAPACITY],
            f"exact-chunk fixture changed: size={size}, chunks={chunks}")
    reader = Client(fixture.replica_port)
    try:
        require(reader.command("GET", "boundary") == payload, "boundary value differs")
    finally:
        reader.close()
    print("[PASS] exact 1 MiB Snapshot completes without an extra chunk", flush=True)


def empty_case(fixture):
    """空数据集也要传完整二进制快照，再接受 ONLINE 增量和客户端流水线。"""
    primary = fixture.start("primary")
    fixture.assert_listener(primary)
    fixture.start("replica", fixture.primary_port)
    fixture.online()
    assert_rdma_snapshot(fixture)
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
    verify_online_mutations(fixture)
    print("[PASS] empty full sync, ONLINE increments, Primary pipeline ordering", flush=True)


def catchup_case(fixture):
    """固定 O → 暂停通知 → 写入增量 → RDMA 全量 → TCP AOF 追赶。"""
    primary = fixture.start("primary")
    fixture.assert_listener(primary)
    writer = Client(fixture.primary_port)
    gate = None
    try:
        # 32 MiB 必须跨多个 RDMA 块，最后一块不足 1 MiB。
        # 写入后的 17 条变更必须从快照边界 O 进入 AOF 追赶。
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
        require(gate.header_ready.wait(timeout=30), "FULLRESYNC_RDMA header not received")
        gate.check()
        require(gate.metadata is not None, "missing Snapshot metadata")
        snapshot_offset, snapshot_size = gate.metadata
        require(snapshot_size >= len(payload) * 512, "Snapshot fixture is unexpectedly small")
        # 暂停的是 RDMA 启动通知，此时 Replica 尚未开始接收快照。
        probe = Client(fixture.replica_port)
        try:
            require(probe.command("PING") == b"PONG",
                    "Replica PING failed during synchronization")

            probe.sock.sendall(frame("HGET", "seed_0"))
            reply = probe.file.readline()

            require(
                reply == b"-LOADING replica synchronization in progress\r\n",
                f"Replica should reject reads before ONLINE: {reply!r}"
            )
        finally:
            probe.close()

        print("[PASS] Replica allows PING and rejects reads before ONLINE",
              flush=True)

        require(writer.command("HSET", "catchup_probe", "written while Snapshot held") == b"OK",
                "write during Snapshot failed")
        for i in range(16):
            require(writer.command("HSET", f"catchup_{i}", f"value_{i}") == b"OK",
                    "catch-up write failed")
        gate.release.set()
        fixture.online()
        gate.check()
        _, chunks = assert_rdma_snapshot(fixture)
        require(len(chunks) > 1 and chunks[-1] < RDMA_CHUNK_CAPACITY,
                "multi-chunk/tail-chunk coverage missing")
        assert_tcp_catchup_only(fixture, gate)
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


def restart_case(fixture):
    """同一工作目录重启副本：忽略损坏旧文件，重新请求当前全量。"""
    primary = fixture.start("primary")
    fixture.assert_listener(primary)
    writer = Client(fixture.primary_port)
    gate = None

    try:
        require(writer.command("HSET", "restart_base", "before") == b"OK",
                "initial write failed")

        replica = fixture.start("replica", fixture.primary_port)
        fixture.online()
        assert_rdma_snapshot(fixture)

        # 停止 Replica，保留它原来的工作目录。
        fixture.stop(replica)
        # Replica 退出后，等待 Primary 清理旧控制连接，再启动新副本。
        primary_log = fixture.root / "primary" / "server.log"
        fixture.wait(
            lambda: "replication: Replica control connection released" in
                    primary_log.read_text(errors="replace"),
            "Primary did not release old Replica control connection",
        )
        # 故意破坏测试目录中的旧文件，验证启动时不会恢复它们。
        (fixture.root / "replica" / "snapshot.db").write_bytes(
            b"invalid old snapshot\n")
        (fixture.root / "replica" / "appendonly.aof").write_bytes(
            b"invalid old AOF\n")

        require(writer.command("HSET", "restart_base", "after") == b"OK",
                "update while Replica stopped failed")
        require(writer.command("HSET", "restart_new", "new-value") == b"OK",
                "new write while Replica stopped failed")

        # 重启 Replica，并暂停这次全量的启动通知。
        gate = SnapshotGate(fixture.primary_port)
        if fixture.replica_port == gate.port:
            fixture.replica_port = free_port()

        fixture.start("replica", gate.port)
        require(gate.header_ready.wait(timeout=30),
                "restarted Replica did not request FULLRESYNC_RDMA")
        gate.check()
        require(gate.metadata is not None,
                "missing fresh Snapshot metadata")

        client = Client(fixture.replica_port)
        try:
            require(client.command("PING") == b"PONG",
                    "restarted Replica PING failed")

            client.sock.sendall(frame("HGET", "restart_base"))
            reply = client.file.readline()
            require(
                reply == b"-LOADING replica synchronization in progress\r\n",
                f"restarted Replica served reads before ONLINE: {reply!r}"
            )

            gate.release.set()
            fixture.online()
            gate.check()
            assert_rdma_snapshot(fixture)
            assert_tcp_catchup_only(fixture, gate)

            require(client.command("HGET", "restart_base") == b"after",
                    "restarted Replica did not load current Primary data")
            require(client.command("HGET", "restart_new") == b"new-value",
                    "restarted Replica is missing new Primary data")
        finally:
            client.close()

        print("[PASS] Replica restart ignores old files and requests fresh full sync",
              flush=True)
    finally:
        writer.close()
        if gate is not None:
            gate.close()

def primary_restart_case(fixture):
    """Primary 本地恢复仍使用 snapshot + checkpoint O 后的 AOF，独立于传输。"""
    primary = fixture.start("primary")
    fixture.assert_listener(primary)

    writer = Client(fixture.primary_port)
    try:
        require(writer.command("HSET", "base", "before") == b"OK",
                "initial Primary write failed")
        require(writer.command("HSET", "stable", "kept") == b"OK",
                "stable key write failed")

        require(writer.command("SAVE") == b"OK",
                "Primary SAVE failed")
        require((fixture.root / "primary" / "snapshot.db").is_file(),
                "Primary SAVE did not create snapshot")

        # 这两条变更发生在 SAVE 之后，必须通过 AOF 恢复。
        require(writer.command("HMOD", "base", "after") == b"OK",
                "post-SAVE modification failed")
        require(writer.command("HSET", "new", "added") == b"OK",
                "post-SAVE insertion failed")
    finally:
        writer.close()

    aof_path = fixture.root / "primary" / "appendonly.aof"
    original_aof = aof_path.read_bytes()

    fixture.stop(primary)
    primary = fixture.start("primary")
    fixture.assert_listener(primary)

    reader = Client(fixture.primary_port)
    try:
        require(reader.command("HGET", "base") == b"after",
                "post-SAVE modification was not recovered")
        require(reader.command("HGET", "stable") == b"kept",
                "Snapshot data was not recovered")
        require(reader.command("HGET", "new") == b"added",
                "post-SAVE insertion was not recovered")
    finally:
        reader.close()

    require(aof_path.read_bytes() == original_aof,
            "Primary recovery unexpectedly modified AOF")

    print("[PASS] Primary restart restores binary Snapshot and later AOF",
          flush=True)

def main():
    global TEST_HOST
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path, help="Linux repository path")
    args = parser.parse_args()
    binary = args.repo.resolve() / "bin" / "kvstore"
    require(sys.platform.startswith("linux"), "run this script on Linux")
    require(binary.is_file() and os.access(binary, os.X_OK), f"build the server first: {binary}")
    require(shutil.which("stdbuf"), "stdbuf is required")
    TEST_HOST = discover_rdma_host()
    print(f"[INFO] real RDMA acceptance on iWARP IPv4 {TEST_HOST}", flush=True)
    root = Path(tempfile.mkdtemp(prefix="91kv-rdma-acceptance-"))
    success = False
    try:
        for name, case in (
            ("empty", empty_case),
            ("exact_chunk", exact_chunk_case),
            ("catchup", catchup_case),
            ("restart", restart_case),
            ("primary_restart", primary_restart_case),
        ):
            case_dir = root / name
            case_dir.mkdir()
            fixture = Fixture(case_dir, binary)
            try:
                case(fixture)
            finally:
                fixture.close()
        success = True
        print("[PASS] RDMA acceptance scenarios: 5 passed; owned server processes stopped", flush=True)
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
