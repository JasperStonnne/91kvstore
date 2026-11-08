#!/usr/bin/env python3
"""真实 BPF -> ring buffer -> ONLINE TCP 复制验收，复用 RDMA 测试工具。

需要加载 BPF 的权限和 ACTIVE iWARP。无权限时明确失败，不把 AOF 降级算作通过。
仅创建临时目录并管理本脚本启动的进程；保留日志和结果供审阅。
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import test_rdma_replication as rdma


class Fixture(rdma.Fixture):
    def launch(self, directory, config):
        return super().launch(directory, config.replace("log_level=info",
                                                       "log_level=debug"))

    def primary_log(self):
        return (self.root / "primary/server.log").read_text(errors="replace")


def payload_ranges(log):
    return [tuple(map(int, row)) for row in re.findall(
        r"eBPF payload range=\[(\d+),(\d+)\) bytes=(\d+)", log)]


def summary(log):
    matches = re.findall(
        r"eBPF summary payload_events=(\d+) payload_bytes=(\d+) "
        r"compensation_ranges=(\d+) compensation_bytes=(\d+) duplicates=(\d+)",
        log)
    rdma.require(matches, "missing eBPF replication summary")
    names = ("events", "bytes", "compensation_ranges", "compensation_bytes",
             "duplicates")
    return dict(zip(names, map(int, matches[-1])))


def normal_case(root, binary):
    fixture = Fixture(root, binary)
    gate = None
    writer = reader = None
    try:
        fixture.start("primary")
        rdma.require("startup attach successful" in fixture.primary_log(),
                     "BPF did not attach; AOF fallback is not an eBPF pass")
        writer = rdma.Client(fixture.primary_port)
        rdma.require(writer.command("HSET", "seed", "snapshot") == b"OK",
                     "seed failed")
        gate = rdma.SnapshotGate(fixture.primary_port)
        replica = fixture.start("replica", gate.port)
        rdma.require(gate.header_ready.wait(10), "Snapshot gate did not open")
        for i in range(3):
            rdma.require(writer.command("HSET", f"during_{i}", "catchup") == b"OK",
                         "write during full sync failed")
        gate.release.set()
        fixture.online()
        gate.check()
        rdma.assert_rdma_snapshot(fixture)
        reader = rdma.Client(fixture.replica_port)
        rdma.require(reader.command("HGET", "seed") == b"snapshot", "seed differs")
        for i in range(3):
            rdma.require(reader.command("HGET", f"during_{i}") == b"catchup",
                         "catch-up value differs")
        catchup_ranges = [tuple(map(int, row)) for row in re.findall(
            r"start AOF catch-up range=\[(\d+),(\d+)\)", fixture.primary_log())]
        fences = list(map(int, re.findall(r"enter ONLINE offset=(\d+)",
                                          fixture.primary_log())))
        rdma.require(len(catchup_ranges) == 1 and len(fences) == 1 and
                     catchup_ranges[0] == (gate.metadata[0], fences[0]) and
                     fences[0] > gate.metadata[0],
                     "nonempty catch-up does not end exactly at the ONLINE fence")

        # 此后才开始写测试命令，确保 payload 计量明确发生在 ONLINE 之后。
        position = (root / "primary/appendonly.aof").stat().st_size
        rdma.require(position == fences[0], "AOF tail and ONLINE fence differ")
        expected_ranges = []

        def write(*fields, captured=True):
            nonlocal position
            encoded = rdma.frame(*fields)
            result = writer.command(*fields)
            end = position + len(encoded)
            if captured:
                expected_ranges.append((position, end, len(encoded)))
            position = end
            return result

        def expect(command, key, value):
            fixture.wait(lambda: reader.command(command, key) == value,
                         f"{command} {key}: Replica differs", timeout=15)

        for prefix in ("", "R", "H", "S"):
            key = f"{prefix or 'A'}_online"
            rdma.require(write(prefix + "SET", key, "first") == b"OK", "SET failed")
            expect(prefix + "GET", key, b"first")
            rdma.require(write(prefix + "MOD", key, "second") == b"OK", "MOD failed")
            expect(prefix + "GET", key, b"second")
            rdma.require(write(prefix + "DEL", key) == 1, "DEL failed")
            expect(prefix + "GET", key, None)

        rdma.require(write("HSET", "split", b"v" * 2500) == b"OK", "split write failed")
        expect("HGET", "split", b"v" * 2500)

        exact = b"x" * 4096
        while len(rdma.frame("HSET", "exact_limit", exact)) != 4096:
            difference = len(rdma.frame("HSET", "exact_limit", exact)) - 4096
            exact = b"x" * (len(exact) - difference)
        rdma.require(write("HSET", "exact_limit", exact) == b"OK", "exact limit failed")
        expect("HGET", "exact_limit", exact)

        writer.sock.sendall(rdma.frame("HDEL", "never_created"))
        rdma.require(writer.file.readline() == b"-ERR DEL failed\r\n",
                     "missing DEL should fail without an append")
        rdma.require((root / "primary/appendonly.aof").stat().st_size == position,
                     "failed DEL unexpectedly appended AOF")
        rdma.require(writer.command("PING") == b"PONG", "PING failed")
        fixture.wait(lambda: len(payload_ranges(fixture.primary_log())) ==
                     len(expected_ranges), "normal eBPF payload records missing")
        rdma.require(payload_ranges(fixture.primary_log()) == expected_ranges,
                     "eBPF payload offsets or lengths differ from the successful writes")
        rdma.require("AOF compensation" not in fixture.primary_log(),
                     "normal ONLINE payload silently came from AOF")
        rdma.require("eBPF chunk bytes=1024" in fixture.primary_log(),
                     "large command did not exercise output segmentation")

        # 一条超限命令必须完整补偿，下一条小命令还要恢复到 eBPF 数据源。
        rdma.require(write("HSET", "oversize", b"L" * 5000,
                           captured=False) == b"OK", "oversize write failed")
        expect("HGET", "oversize", b"L" * 5000)
        rdma.require(write("HSET", "after_gap", "back_to_events") == b"OK",
                     "post-gap write failed")
        expect("HGET", "after_gap", b"back_to_events")
        fixture.wait(lambda: len(payload_ranges(fixture.primary_log())) ==
                     len(expected_ranges), "post-gap eBPF event missing")
        rdma.require(payload_ranges(fixture.primary_log()) == expected_ranges,
                     "post-gap event offsets differ")
        fixture.wait(lambda: "oversize=1" in fixture.primary_log(),
                     "oversize counter missing")

        reader.close()
        reader = None
        fixture.stop(replica)
        # 闸门代理还持有 Primary 连接，关闭后 Primary 才能观察到断线。
        gate.close()
        gate = None
        fixture.wait(lambda: "eBPF summary" in fixture.primary_log(),
                     "disconnect summary missing")
        result = summary(fixture.primary_log())
        rdma.require(result["events"] == len(expected_ranges), "event count differs")
        rdma.require(result["bytes"] == sum(x[2] for x in expected_ranges),
                     "eBPF payload byte count differs")
        rdma.require(result["compensation_bytes"] ==
                     len(rdma.frame("HSET", "oversize", b"L" * 5000)),
                     "oversize AOF compensation byte count differs")
        rdma.require(result["compensation_ranges"] == 1, "unexpected compensation")
        rdma.require(result["duplicates"] == 4, "catch-up-covered events not skipped")
        result.update(catchup_ranges=catchup_ranges, online_fence=fences[0])
        print("[PASS] ONLINE four engines, exact offsets, 4 KiB limit, output segmentation, "
              "failed write exclusion and explicit oversize compensation", flush=True)
        return result
    finally:
        if reader is not None:
            reader.close()
        if writer is not None:
            writer.close()
        if gate is not None:
            gate.close()
        fixture.close()


def compile_bpf_variant(root, repo, header, legacy=False):
    (root / "ebpf_event.h").write_text(header)
    source = (repo / "src/replication/ebpf_capture.bpf.c").read_text()
    if legacy:
        source = source.replace("    event->version = KVS_EBPF_EVENT_VERSION;\n", "")
    source_path = root / "variant.bpf.c"
    source_path.write_text(source)
    architecture = {"aarch64": "arm64", "x86_64": "x86"}[os.uname().machine]
    multiarch = subprocess.check_output(["gcc", "-dumpmachine"], text=True).strip()
    output = root / "capture.bpf.o"
    subprocess.run([
        "clang", "-target", "bpf", f"-D__TARGET_ARCH_{architecture}", "-O2", "-g",
        "-Wall", "-Wextra", "-Werror", f"-I{root}", f"-I{repo / 'include'}",
        f"-I/usr/include/{multiarch}", "-c", str(source_path), "-o", str(output),
    ], check=True)
    return output


def old_events_case(root, binary):
    fixture = Fixture(root, binary)
    writer = reader = None
    total = 600
    try:
        fixture.start("primary")
        rdma.require("startup attach successful" in fixture.primary_log(), "BPF attach failed")
        writer = rdma.Client(fixture.primary_port)
        for i in range(total):
            rdma.require(writer.command("HSET", f"old_{i}", f"value_{i}") == b"OK",
                         "pre-Replica write failed")
        fence = (root / "primary/appendonly.aof").stat().st_size
        replica = fixture.start("replica", fixture.primary_port)
        fixture.online()
        # 队列容量可确定保留多少旧事件，其余失败已经被快照覆盖。
        event_bytes = 24 + 4096
        ring_record_bytes = (event_bytes + 8 + 7) // 8 * 8
        retained = ((1024 * 1024) - 1) // ring_record_bytes
        dropped = total - retained
        fixture.wait(lambda: f"ring_full={dropped} " in fixture.primary_log(),
                     "pre-ONLINE queue accounting differs")
        reader = rdma.Client(fixture.replica_port)
        for i in range(total):
            rdma.require(reader.command("HGET", f"old_{i}") == f"value_{i}".encode(),
                         f"Snapshot key old_{i} differs")
        encoded = rdma.frame("HSET", "fresh_after_old", "new")
        rdma.require(writer.command("HSET", "fresh_after_old", "new") == b"OK",
                     "post-fence write failed")
        fixture.wait(lambda: reader.command("HGET", "fresh_after_old") == b"new",
                     "fresh post-fence event missing")
        fixture.wait(lambda: len(payload_ranges(fixture.primary_log())) == 1,
                     "fresh post-fence payload record missing")
        rdma.require(payload_ranges(fixture.primary_log()) ==
                     [(fence, fence + len(encoded), len(encoded))],
                     "fresh event does not start exactly at the full-sync fence")
        rdma.require("AOF compensation" not in fixture.primary_log(),
                     "covered pre-ONLINE losses caused duplicate compensation")
        reader.close()
        reader = None
        fixture.stop(replica)
        fixture.wait(lambda: "eBPF summary" in fixture.primary_log(),
                     "old-event summary missing")
        result = summary(fixture.primary_log())
        rdma.require(result["duplicates"] == retained and result["events"] == 1,
                     "old-event skip boundary/count differs")
        result.update(pre_online_writes=total, pre_online_dropped=dropped, fence=fence)
        print("[PASS] 600 pre-Replica writes: exact old-event skip count and fresh event fence",
              flush=True)
        return result
    finally:
        if reader is not None:
            reader.close()
        if writer is not None:
            writer.close()
        fixture.close()


def interrupted_consumer_case(root, repo, binary):
    library = root / "consumer_fault.so"
    marker = root / "interrupt"
    subprocess.run([
        "gcc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror",
        str(repo / "tests/ebpf_consumer_fault.c"), "-ldl", "-lbpf", "-o", str(library),
    ], check=True)
    saved = {key: os.environ.get(key) for key in ("LD_PRELOAD", "KVS_EBPF_FAULT_FILE")}
    os.environ["LD_PRELOAD"] = str(library)
    os.environ["KVS_EBPF_FAULT_FILE"] = str(marker)
    fixture = Fixture(root, binary)
    writer = reader = None
    try:
        fixture.start("primary")
        rdma.require("startup attach successful" in fixture.primary_log(), "BPF attach failed")
        replica = fixture.start("replica", fixture.primary_port)
        fixture.online()
        writer = rdma.Client(fixture.primary_port)
        reader = rdma.Client(fixture.replica_port)
        rdma.require(writer.command("HSET", "before_interrupt", "event") == b"OK",
                     "baseline write failed")
        fixture.wait(lambda: reader.command("HGET", "before_interrupt") == b"event",
                     "baseline eBPF event did not replicate")
        fixture.wait(lambda: len(payload_ranges(fixture.primary_log())) == 1,
                     "baseline eBPF event provenance missing")

        marker.touch()
        records = [(f"interrupted_{i}", f"value_{i}") for i in range(20)]
        for key, value in records:
            rdma.require(writer.command("HSET", key, value) == b"OK",
                         "write during consumption failure failed")
        fixture.wait(lambda: "eBPF consumer failed error=-5; ONLINE source=AOF" in
                     fixture.primary_log(), "consumer failure did not trigger explicit AOF fallback")
        for key, value in records:
            fixture.wait(lambda key=key, value=value:
                         reader.command("HGET", key) == value.encode(),
                         f"consumer failure lost {key}")
        expected = rdma.frame("HSET", "before_interrupt", "event") + b"".join(
            rdma.frame("HSET", key, value) for key, value in records)
        rdma.require((root / "primary/appendonly.aof").read_bytes() == expected,
                     "Primary AOF differs after consumption failure")
        rdma.require((root / "replica/appendonly.aof").read_bytes() == expected,
                     "AOF replay after consumer interruption is incomplete or duplicated")
        rdma.require(len(payload_ranges(fixture.primary_log())) == 1,
                     "post-failure AOF data misreported as BPF payload")
        reader.close()
        reader = None
        fixture.stop(replica)
        fixture.wait(lambda: "eBPF summary" in fixture.primary_log(),
                     "consumer failure summary missing")
        result = summary(fixture.primary_log())
        result.update(replayed_commands=len(records),
                      replayed_bytes=sum(len(rdma.frame("HSET", k, v)) for k, v in records))
        print("[PASS] deterministic consumer -EIO with queued event: complete AOF fallback replay",
              flush=True)
        return result
    finally:
        if reader is not None:
            reader.close()
        if writer is not None:
            writer.close()
        fixture.close()
        for key, value in saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def rejected_version_case(root, repo, binary, legacy):
    header = (repo / "include/ebpf_event.h").read_text()
    expected_version = 0 if legacy else 99
    if legacy:
        old = """struct kvs_ebpf_event {
    __u32 version;
    __u32 length;
    __u64 start;
    __u64 end;
    unsigned char payload[KVS_EBPF_MAX_PAYLOAD];
};"""
        new = """struct kvs_ebpf_event {
    __u64 start;
    __u64 end;
    __u32 length;
    unsigned char payload[KVS_EBPF_MAX_PAYLOAD];
};"""
        rdma.require(old in header, "legacy fixture layout changed")
        header = header.replace(old, new)
    else:
        header = header.replace("#define KVS_EBPF_EVENT_VERSION 1",
                                "#define KVS_EBPF_EVENT_VERSION 99")
    os.environ["KVS_EBPF_OBJECT"] = str(compile_bpf_variant(root, repo, header, legacy))
    fixture = Fixture(root, binary)
    writer = reader = None
    try:
        fixture.start("primary")
        rdma.require("startup attach successful" in fixture.primary_log(), "BPF attach failed")
        replica = fixture.start("replica", fixture.primary_port)
        fixture.online()
        writer = rdma.Client(fixture.primary_port)
        reader = rdma.Client(fixture.replica_port)
        records = [("rejected", "from_aof"), ("after_rejection", "still_ok")]
        for key, value in records:
            rdma.require(writer.command("HSET", key, value) == b"OK", "write failed")
        fixture.wait(lambda: f"unsupported event version={expected_version} expected=1" in
                     fixture.primary_log(), "incompatible event was not explicitly rejected")
        fixture.wait(lambda: "eBPF consumer failed" in fixture.primary_log() and
                     "ONLINE source=AOF" in fixture.primary_log(), "missing explicit fallback")
        for key, value in records:
            fixture.wait(lambda key=key, value=value:
                         reader.command("HGET", key) == value.encode(),
                         "version rejection lost a command")
        expected = b"".join(rdma.frame("HSET", k, v) for k, v in records)
        rdma.require((root / "replica/appendonly.aof").read_bytes() == expected,
                     "version rejection replay differs")
        rdma.require(not payload_ranges(fixture.primary_log()),
                     "rejected version incorrectly counted as eBPF payload")
        print(f"[PASS] incompatible event version {expected_version} rejected; explicit AOF replay",
              flush=True)
        return {"rejected_version": expected_version, "replayed_commands": len(records)}
    finally:
        if reader is not None:
            reader.close()
        if writer is not None:
            writer.close()
        fixture.close()


def pressure_case(root, repo, binary):
    # 仅测试变体缩小队列，生产默认容量保持 1 MiB。
    header = (repo / "include/ebpf_event.h").read_text().replace(
        "#define KVS_EBPF_RING_BYTES (1024 * 1024)",
        "#define KVS_EBPF_RING_BYTES 8192")
    os.environ["KVS_EBPF_OBJECT"] = str(compile_bpf_variant(root, repo, header))
    fixture = Fixture(root, binary)
    writer = reader = None
    try:
        fixture.start("primary")
        rdma.require("startup attach successful" in fixture.primary_log(),
                     "pressure BPF failed to attach")
        replica = fixture.start("replica", fixture.primary_port)
        fixture.online()
        writer = rdma.Client(fixture.primary_port)
        reader = rdma.Client(fixture.replica_port)
        total = 100
        writer.sock.sendall(b"".join(
            rdma.frame("HSET", f"pressure_{i}", f"value_{i}") for i in range(total)))
        for _ in range(total):
            rdma.require(writer.reply() == b"OK", "pressure write failed")

        # 先验证最后一条，证明队列末尾丢失也能补偿，不依赖后续新命令唤醒。
        fixture.wait(lambda: reader.command("HGET", "pressure_99") == b"value_99",
                     "final pressure command missing without later writes")
        for i in range(total):
            rdma.require(reader.command("HGET", f"pressure_{i}") == f"value_{i}".encode(),
                         f"pressure value {i} differs")
        fixture.wait(lambda: re.search(r"ring_full=[1-9]\d*", fixture.primary_log()),
                     "ring pressure did not produce an audited loss")
        reader.close()
        reader = None
        fixture.stop(replica)
        fixture.wait(lambda: "eBPF summary" in fixture.primary_log(),
                     "pressure summary missing")
        result = summary(fixture.primary_log())
        rdma.require(result["compensation_bytes"] > 0, "pressure AOF compensation missing")
        expected = b"".join(rdma.frame("HSET", f"pressure_{i}", f"value_{i}")
                            for i in range(total))
        rdma.require((root / "primary/appendonly.aof").read_bytes() == expected and
                     (root / "replica/appendonly.aof").read_bytes() == expected,
                     "overflow replay duplicated or lost command bytes")
        rdma.require(result["bytes"] + result["compensation_bytes"] == len(expected),
                     "overflow source byte accounting differs")
        print("[PASS] real ring-buffer overflow: all 100 writes restored, including final write",
              flush=True)
        return result
    finally:
        if reader is not None:
            reader.close()
        if writer is not None:
            writer.close()
        fixture.close()


def main():
    rdma.require(os.geteuid() == 0,
                 "Run the BPF-specific acceptance with sudo; ordinary make test needs no BPF load")
    repo = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    binary = repo / "bin/kvstore-ebpf"
    root = Path(tempfile.mkdtemp(prefix="91kv-ebpf-acceptance."))
    root.chmod(0o755)
    print(f"[INFO] evidence directory: {root}", flush=True)
    rdma.TEST_HOST = rdma.discover_rdma_host()
    original_object = os.environ.get("KVS_EBPF_OBJECT")
    os.environ["KVS_EBPF_OBJECT"] = str(repo / "build/ebpf_capture.bpf.o")
    try:
        normal_root = root / "normal"
        normal_root.mkdir()
        normal = normal_case(normal_root, binary)
        pressure = []
        for repetition in range(3):
            pressure_root = root / f"pressure_{repetition + 1}"
            pressure_root.mkdir()
            pressure.append(pressure_case(pressure_root, repo, binary))
        os.environ["KVS_EBPF_OBJECT"] = str(repo / "build/ebpf_capture.bpf.o")
        old_root = root / "old_events"
        old_root.mkdir()
        old_events = old_events_case(old_root, binary)
        interrupted_root = root / "consumer_interruption"
        interrupted_root.mkdir()
        interruption = interrupted_consumer_case(interrupted_root, repo, binary)
        rejected = []
        for legacy in (True, False):
            version_root = root / ("legacy_version" if legacy else "unknown_version")
            version_root.mkdir()
            rejected.append(rejected_version_case(version_root, repo, binary, legacy))
        (root / "summary.json").write_text(json.dumps(
            {"normal": normal, "pressure": pressure, "old_events": old_events,
             "consumer_interruption": interruption, "version_rejection": rejected,
             "scope": "single VM, two processes; SIW functionality only"},
            ensure_ascii=False, indent=2) + "\n")
        print("[PASS] eBPF ONLINE acceptance complete; owned processes stopped", flush=True)
    finally:
        if original_object is None:
            os.environ.pop("KVS_EBPF_OBJECT", None)
        else:
            os.environ["KVS_EBPF_OBJECT"] = original_object


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"[FAIL] {error}", file=sys.stderr, flush=True)
        sys.exit(1)
