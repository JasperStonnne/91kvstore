#!/usr/bin/env python3
"""Linux 上的真实 RDMA 全量同步验收（由 make test-rdma 调用）。

Primary 与 Replica 在同一台机器的 iWARP 网卡地址上通信，不模拟 RDMA。
TCP 仍负责握手、控制消息和 复制增量；快照内容必须通过 RDMA 发送。
自动寻找 ACTIVE iWARP 网卡；也可用 RDMA_TEST_IP 指定其 IPv4 地址。
设备缺失或同步失败会明确报错，不回退到 TCP 全量或将跳过记为通过。
"""
import argparse
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
        # 逐块字节与 ACK 断言需要 DEBUG；正式默认 INFO 不输出这些细节。
        self.log_level = "debug"
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
            f"log_level={self.log_level}\npersistence_mode=none\n"
        )
        if role == "primary":
            config += f"rdma_port={self.rdma_port}\n"
        if role == "replica":
            config += f"primary_host={TEST_HOST}\nprimary_port={upstream}\n"
        process, log_path = self.launch(directory, config)
        self.wait(lambda: f"listen port : {port}" in log_path.read_text(errors="replace"),
                  f"{role} did not start")
        return process

    def launch(self, directory, config):
        """只启动并登记所有权；失败用例可以等待退出，不要求出现监听日志。"""
        directory.mkdir(exist_ok=True)
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
        return process, log_path

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
    """只代理 TCP 控制和复制增量，不接触 RDMA 快照数据。

    收到 FULLRESYNC_RDMA 时，Primary 已经生成快照并固定边界 O。
    暂停该通知，可确定性地在 O 之后写入增量，再放行真实 RDMA 全量。
    不靠 sleep 或网速猜测同步时机，避免快照很小时测试偶发失效。
    """
    def __init__(self, primary_port, advertised_rdma_port=None,
                 connection_timeout=30):
        self.primary_port = primary_port
        # 仅失败测试覆盖通知端口，模拟配置/发布错误；不接触实际 RDMA 数据。
        self.advertised_rdma_port = advertised_rdma_port
        self.full_sync_id = None
        self.listener = socket.socket()
        self.listener.bind((TEST_HOST, 0))
        self.listener.listen(1)
        self.listener.settimeout(connection_timeout)
        self.port = self.listener.getsockname()[1]
        self.header_ready = threading.Event()
        self.release = threading.Event()
        self.stopped = threading.Event()
        self.errors = []
        self.sockets = [self.listener]
        self.snapshot_size = None
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
                    re.fullmatch(rb"[0-9a-f]{40}", parts[4]) is not None,
                    f"invalid replication/full-sync ID: {header!r}")
            require(0 < int(parts[3]) <= 65535, "invalid RDMA port")
            self.full_sync_id = parts[4]
            self.snapshot_size = int(parts[2])
            self.stream_offset = int(parts[5])
            if self.advertised_rdma_port is not None:
                parts[3] = str(self.advertised_rdma_port).encode()
                header = b" ".join(parts) + b"\r\n"
            require(self.snapshot_size > 0,
                    "invalid Snapshot size")
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
                # 只统计全量通知之后的 TCP 字节，稍后校验恰好是 增量 + ONLINE。
                self.forward(data, downstream)
        except Exception as exc:
            if not self.stopped.is_set():
                self.errors.append(str(exc))
                self.header_ready.set()

    def forward(self, data, downstream):
        """默认原样转发；故障测试子类可按精确字节边界截断。"""
        self.forwarded_bytes += len(data)
        downstream.sendall(data)

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


def primary_snapshot_path(fixture):
    return fixture.root / "primary/replication.snapshot.tmp"


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
    require(sent and sent == received, f"RDMA chunk sequence differs: {sent} / {received}")
    require(all(0 < n <= RDMA_CHUNK_CAPACITY for n in sent), "invalid RDMA chunk size")
    cumulative = []
    total = 0
    for length in sent:
        total += length
        cumulative.append(total)
    require(acknowledgements == cumulative,
            f"cumulative ACK differs: {acknowledgements} / {cumulative}")
    require(re.search(rf"Snapshot installed, enter CATCH_UP offset=\d+ size={total} fd=",
                      replica_log),
            "Replica did not load the advertised number of Snapshot bytes")
    require(not primary_snapshot_path(fixture).exists(),
            "Primary kept the temporary Snapshot after final ACK")
    require(f"RDMA Snapshot fully acknowledged bytes={total}" in session,
            "missing final Snapshot ACK")
    require("Primary RDMA closed, start TCP catch-up" in session and
            "Replica RDMA closed, resume TCP catch-up" in replica_log,
            "RDMA did not close normally before TCP catch-up")
    require(not (fixture.root / "replica/snapshot.db.replication.tmp").exists(),
            "Replica kept the temporary Snapshot after loading")
    print(f"[PASS] real RDMA Snapshot: {total} bytes, {len(sent)} chunks, cumulative ACK and temporary file cleanup",
          flush=True)
    return total, sent


def assert_tcp_catchup_only(fixture, gate):
    """全量通知后，TCP 字节数只能等于 [O, ONLINE offset) 的复制命令加 ONLINE 行。"""
    log = (fixture.root / "replica/server.log").read_text(errors="replace")
    offsets = re.findall(r"enter ONLINE offset=(\d+)", log)
    require(offsets, "missing ONLINE offset")
    online_offset = int(offsets[-1])
    start = gate.stream_offset
    expected = online_offset - start + len(f"ONLINE {online_offset}\r\n".encode())
    require(gate.forwarded_bytes == expected,
            f"unexpected TCP payload: {gate.forwarded_bytes} bytes; cache + ONLINE needs {expected}")


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
    print("[PASS] ONLINE insertion, modification and deletion", flush=True)


def exact_chunk_case(fixture):
    """恰好 1 MiB 的快照：最终块不能多发，也不能等一个不存在的下一块。"""
    fixture.start("primary")
    writer = Client(fixture.primary_port)
    # v3 头及段表 48 + 长度 8 + key 8 + 整文件 CRC 4 = 68 字节。
    payload = b"z" * (RDMA_CHUNK_CAPACITY - 68)
    try:
        require(writer.command("SET", "boundary", payload) == b"OK", "boundary seed failed")
    finally:
        writer.close()
    fixture.start("replica", fixture.primary_port)
    fixture.online()
    size, chunks = assert_rdma_snapshot(fixture)
    require(not (fixture.root / "replica/snapshot.db").exists(),
            "replication created a local persistence Snapshot")
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
    """固定 O → 暂停通知 → 写入增量 → RDMA 全量 → TCP 增量追赶。"""
    primary = fixture.start("primary")
    fixture.assert_listener(primary)
    writer = Client(fixture.primary_port)
    gate = None
    try:
        # 32 MiB 必须跨多个 RDMA 块，最后一块不足 1 MiB。
        # 写入后的 17 条变更必须从快照边界 O 进入增量追赶。
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
        require(gate.snapshot_size is not None, "missing Snapshot metadata")
        snapshot_size = gate.snapshot_size
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
            r"start cache catch-up range=\[(\d+),(\d+)\)", log
        )]
        require(any(a == gate.stream_offset and b > a for a, b in ranges),
                f"nonempty Snapshot-to-cache catch-up was not demonstrated: {ranges}")
        print(f"[PASS] nonempty cache catch-up {ranges}; all 512 Snapshot keys and 17 new keys verified",
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
        old_snapshot = fixture.root / "replica" / "snapshot.db"
        old_snapshot.write_bytes(b"invalid old snapshot\n")
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
        require(gate.snapshot_size is not None,
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
            require(old_snapshot.read_bytes() == b"invalid old snapshot\n",
                    "replication replaced the existing local Snapshot")

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

def interrupted_restart_case(fixture):
    """真实 RDMA 已写入首块后终止副本；从同一目录重启必须重新全量。"""
    fixture.start("primary")
    writer = Client(fixture.primary_port)
    gate = None
    try:
        # 16 MiB 跨多个块，日志轮询能在首块与最终安装之间确定中断点。
        payload = b"r" * (64 * 1024)
        for first in range(0, 256, 8):
            writer.sock.sendall(b"".join(
                frame("HSET", f"interrupted_{i}", payload)
                for i in range(first, first + 8)
            ))
            require(all(writer.reply() == b"OK" for _ in range(8)), "seeding failed")
        gate = SnapshotGate(fixture.primary_port)
        if fixture.replica_port == gate.port:
            fixture.replica_port = free_port()
        replica = fixture.start("replica", gate.port)
        require(gate.header_ready.wait(10), "interrupted-sync notification missing")
        gate.check()
        first_id = gate.full_sync_id
        expected_size = gate.snapshot_size
        gate.release.set()
        replica_log = fixture.root / "replica/server.log"
        fixture.wait(lambda: "Replica wrote Snapshot chunk" in
                     replica_log.read_text(errors="replace"),
                     "Replica never received first RDMA chunk", timeout=10)
        fixture.stop(replica)
        first_log = replica_log.read_text(errors="replace")
        require("Snapshot installed" not in first_log and "enter ONLINE" not in first_log,
                "test interrupted too late: Snapshot was already installed")
        temporary = fixture.root / "replica/snapshot.db.replication.tmp"
        require(temporary.is_file() and 0 < temporary.stat().st_size < expected_size,
                "test did not leave a partially transferred Snapshot")
        require(not (fixture.root / "replica/snapshot.db").exists(),
                "partial Snapshot must not become the installed Snapshot")
        (fixture.root / "replica/interrupted-server.log").write_text(first_log)
        # 代理持有 TCP 上游，必须先关闭代理，再等待 Primary 清理这一轮。
        gate.close()
        gate = None
        primary_log = fixture.root / "primary/server.log"
        fixture.wait(lambda: "Replica control connection released" in
                     primary_log.read_text(errors="replace"),
                     "Primary did not cancel interrupted sync", timeout=10)
        require(not primary_snapshot_path(fixture).exists(),
                "interrupted sync leaked the Primary Snapshot")
        primary_snapshot_path(fixture).write_bytes(b"stale temporary snapshot")
        require(writer.command("HMOD", "interrupted_0", "latest") == b"OK",
                "Primary update while Replica stopped failed")
        require(writer.command("HSET", "interrupted_new", "new-value") == b"OK",
                "Primary insertion while Replica stopped failed")
        gate = SnapshotGate(fixture.primary_port)
        if fixture.replica_port == gate.port:
            fixture.replica_port = free_port()
        fixture.start("replica", gate.port)
        require(gate.header_ready.wait(10), "restarted Replica did not request full sync")
        gate.check()
        require(gate.full_sync_id != first_id, "restart reused the old full-sync ID")
        gate.release.set()
        fixture.online()
        assert_rdma_snapshot(fixture)
        reader = Client(fixture.replica_port)
        try:
            for i in range(256):
                expected = b"latest" if i == 0 else payload
                require(reader.command("HGET", f"interrupted_{i}") == expected,
                        f"restarted Snapshot key {i} differs")
            require(reader.command("HGET", "interrupted_new") == b"new-value",
                    "fresh Snapshot omitted offline writes")
        finally:
            reader.close()
        require(not temporary.exists(), "successful restart left partial Snapshot file")
        print("[PASS] mid-transfer Replica stop leaves no installed Snapshot; restart performs fresh RDMA full sync",
              flush=True)
    finally:
        writer.close()
        if gate is not None:
            gate.close()


def listener_failure_case(fixture):
    """实际 RDMA 端口已占用时，另一 Primary 必须明确报错并快速退出。"""
    fixture.start("primary")
    config = (
        f"role=primary\nbind_ip={TEST_HOST}\nservice_port={fixture.replica_port}\n"
        f"rdma_port={fixture.rdma_port}\n"
        "allocator=pool\nlog_level=info\npersistence_mode=none\n"
    )
    process, log_path = fixture.launch(fixture.root / "invalid-primary", config)
    try:
        status = process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        raise RuntimeError("Primary hung on an occupied RDMA port") from None
    require(status != 0, "Primary unexpectedly accepted occupied RDMA port")
    log = log_path.read_text(errors="replace")
    require(f"failed to start RDMA listener {TEST_HOST}:{fixture.rdma_port}:" in log,
            f"startup failure lacks RDMA endpoint diagnostic: {log}")
    require("listen port :" not in log, "Primary served clients without RDMA listener")
    fixture.stop(process)
    print("[PASS] occupied RDMA port: Primary exits within 10s with endpoint diagnostic", flush=True)


def check_failed_handshake(fixture, silent_peer):
    """分别检查连接拒绝，以及 TCP 连通但不回应 RDMA 的建连超时。"""
    gate = None
    replica = None
    primary_log = fixture.root / "primary/server.log"
    released_before = primary_log.read_text(errors="replace").count(
        "Replica control connection released")
    # 普通 TCP 监听者不处理 RDMA 握手：TCP 可连通，但不会有 CM ESTABLISHED。
    # 用它确定性验证生产建连截止时间，而非只等待内核给出连接拒绝。
    with socket.socket() as unused:
        unused.bind((TEST_HOST, 0))
        if silent_peer:
            unused.listen(1)
        wrong_port = unused.getsockname()[1]
        if fixture.replica_port == wrong_port:
            fixture.replica_port = free_port()
        try:
            gate = SnapshotGate(fixture.primary_port, advertised_rdma_port=wrong_port)
            if fixture.replica_port == gate.port:
                fixture.replica_port = free_port()
            replica = fixture.start("replica", gate.port)
            require(gate.header_ready.wait(10), "bad-port notification missing")
            gate.check()
            gate.release.set()
            log_path = fixture.root / "replica/server.log"
            fixture.wait(lambda: "Replica RDMA CM poll failed" in
                         log_path.read_text(errors="replace"),
                         "Replica hung waiting for invalid RDMA endpoint", timeout=10)
            fixture.wait(lambda: "synchronization cancelled" in
                         log_path.read_text(errors="replace"),
                         "Replica failed to leave unsuccessful handshake", timeout=10)
            log = log_path.read_text(errors="replace")
            require(f"host={TEST_HOST} port={wrong_port}:" in log,
                    "RDMA handshake diagnostic omits the failed endpoint")
            expected_error = "Connection timed out" if silent_peer else "Connection refused"
            require(expected_error in log,
                    f"RDMA handshake diagnostic lacks expected error: {expected_error}")
            require("enter ONLINE" not in log and "Snapshot installed" not in log,
                    "failed RDMA handshake advanced replication state")
            require(not (fixture.root / "replica/snapshot.db.replication.tmp").exists(),
                    "failed handshake leaked a temporary Snapshot")
            reader = Client(fixture.replica_port)
            try:
                require(reader.command("PING") == b"PONG", "failed handshake blocked PING")
                reader.sock.sendall(frame("HGET", "missing"))
                require(reader.file.readline() == b"-LOADING replica synchronization in progress\r\n",
                        "failed handshake allowed data reads")
            finally:
                reader.close()
            mode = "silent peer timeout" if silent_peer else "connection refused"
            print(f"[PASS] invalid RDMA port ({mode}): bounded failure, endpoint log, no ONLINE, PING usable",
                  flush=True)
        finally:
            if gate is not None:
                gate.close()
            if replica is not None:
                fixture.stop(replica)
    fixture.wait(lambda: primary_log.read_text(errors="replace").count(
                 "Replica control connection released") > released_before,
                 "Primary did not release the failed handshake", timeout=10)
    require(not primary_snapshot_path(fixture).exists(),
            "failed handshake leaked the Primary Snapshot")


def handshake_failure_case(fixture):
    """错误通知端口的即时拒绝和静默握手都必须退出同步，不得挂住事件循环。"""
    fixture.start("primary")
    check_failed_handshake(fixture, silent_peer=False)
    check_failed_handshake(fixture, silent_peer=True)


def info_logging_case(fixture):
    """INFO 保留复制状态，隐藏逐块数据、ACK 和逐事件细节。"""
    fixture.log_level = "info"
    fixture.start("primary")
    writer = Client(fixture.primary_port)
    try:
        require(writer.command("HSET", "info_probe", "before") == b"OK", "INFO seed failed")
    finally:
        writer.close()
    fixture.start("replica", fixture.primary_port)
    fixture.online()
    verify_online_mutations(fixture)
    for role in ("primary", "replica"):
        log = (fixture.root / role / "server.log").read_text(errors="replace")
        require("enter ONLINE" in log, f"{role}: INFO state transition missing")
        for detail in ("submitted Snapshot chunk", "Snapshot chunk send completed",
                       "wrote Snapshot chunk", "received Snapshot ACK",
                       "submitted Snapshot ACK", "eBPF payload range="):
            require(detail not in log, f"{role}: DEBUG detail leaked at INFO: {detail}")
    reader = Client(fixture.replica_port)
    try:
        require(reader.command("HGET", "info_probe") == b"before", "INFO Snapshot data missing")
    finally:
        reader.close()
    print("[PASS] INFO retains state transitions and hides per-chunk/ACK detail", flush=True)

def corrupt_snapshot_case(fixture):
    """仅破坏本测试临时文件，验证 RDMA 收齐后不会确认损坏快照。"""
    fixture.start("primary")
    writer = Client(fixture.primary_port)
    gate = reader = None
    try:
        require(writer.command("HSET", "crc_probe", "kept") == b"OK", "seed failed")
        gate = SnapshotGate(fixture.primary_port)
        if gate.port == fixture.replica_port:
            fixture.replica_port = free_port()
        fixture.start("replica", gate.port)
        require(gate.header_ready.wait(10), "Snapshot header missing")
        gate.check()
        path = primary_snapshot_path(fixture)
        # 使用已打开文件的同一 inode，保持大小和握手信息一致。
        with path.open("r+b") as snapshot:
            require(snapshot.read(13) == b"91KVSNAP\0\0\0\3\0", "replication must write v3/file")
            snapshot.seek(-1, 2)
            original = snapshot.read(1)
            snapshot.seek(-1, 2)
            snapshot.write(bytes([original[0] ^ 1]))
        gate.release.set()
        replica_log = fixture.root / "replica/server.log"
        fixture.wait(lambda: "Replica control connection closed" in replica_log.read_text(errors="replace"),
                     "corrupt full sync did not fail", timeout=15)
        primary_log = fixture.root / "primary/server.log"
        fixture.wait(lambda: "Replica control connection released" in primary_log.read_text(errors="replace"),
                     "Primary retained corrupt session", timeout=15)
        replica_text = replica_log.read_text(errors="replace")
        primary_text = primary_log.read_text(errors="replace")
        require("Primary submitted Snapshot chunk" in primary_text and
                "Snapshot validation/load failed" in replica_text,
                "RDMA Snapshot did not reach strict validation/load")
        require("Snapshot installed" not in replica_text and "enter ONLINE" not in replica_text,
                "corrupt Snapshot was installed")
        require("RDMA Snapshot fully acknowledged" not in primary_text,
                "Primary received false load-completion confirmation")
        require(not (fixture.root / "replica/snapshot.db.replication.tmp").exists(),
                "Replica kept corrupt temporary file")
        require(not primary_snapshot_path(fixture).exists(), "Primary kept corrupt temporary file")
        reader = Client(fixture.replica_port)
        require(reader.command("PING") == b"PONG", "Replica PING unavailable")
        reader.sock.sendall(frame("HGET", "crc_probe"))
        require(reader.file.readline() == b"-LOADING replica synchronization in progress\r\n",
                "corrupt Replica served reads")
        print("[PASS] real RDMA corrupt v3/file Snapshot rejected; no load ACK/ONLINE; temp files cleaned", flush=True)
    finally:
        if reader is not None:
            reader.close()
        writer.close()
        if gate is not None:
            gate.close()


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
            ("info_logging", info_logging_case),
            ("empty", empty_case),
            ("corrupt_snapshot", corrupt_snapshot_case),
            ("exact_chunk", exact_chunk_case),
            ("catchup", catchup_case),
            ("restart", restart_case),
            ("interrupted_restart", interrupted_restart_case),
            ("listener_failure", listener_failure_case),
            ("handshake_failure", handshake_failure_case),
        ):
            case_dir = root / name
            case_dir.mkdir()
            fixture = Fixture(case_dir, binary)
            try:
                case(fixture)
            finally:
                fixture.close()
        success = True
        print("[PASS] RDMA acceptance scenarios: 9 passed; owned server processes stopped", flush=True)
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
