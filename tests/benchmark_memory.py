#!/usr/bin/env python3
"""91kvstore memory comparison. No SAVE, AOF, replication, or throughput claim.

Copy this file to tests/benchmark_memory.py in the Linux repository.
Default: a 10,000-record, one-round pilot of all three modes.
All artifacts are retained in a new result directory under /tmp.
"""

import argparse
import base64
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
MODES = ("system", "pool", "jemalloc")
ENGINES = (
    ("array", b"SET", b"GET", b"DEL"),
    ("hash", b"HSET", b"HGET", b"HDEL"),
    ("rbtree", b"RSET", b"RGET", b"RDEL"),
    ("skiplist", b"SSET", b"SGET", b"SDEL"),
)
CLEAR_ENV = (
    "LD_PRELOAD", "LD_AUDIT", "LD_DEBUG", "LD_DEBUG_OUTPUT",
    "MALLOC_CONF", "GLIBC_TUNABLES", "MALLOC_ARENA_MAX",
    "MALLOC_MMAP_THRESHOLD_", "MALLOC_TRIM_THRESHOLD_", "MALLOC_PERTURB_",
)


def make_record(index):
    if not 0 <= index < 1_000_000:
        raise ValueError("record index out of range")
    digest = hashlib.sha256(f"91kv-stage8:{index}".encode("ascii")).digest()
    key = f"{index:08x}".encode("ascii") + base64.b85encode(digest)[:24]
    value = digest.hex().encode("ascii") * 2
    return key, value


def encode_command(*fields):
    return b"*%d\r\n" % len(fields) + b"".join(
        b"$%d\r\n" % len(field) + field + b"\r\n" for field in fields
    )


def make_plan(total, array_capacity):
    array_count = min(array_capacity, total // 4)
    common, remainder = divmod(total - array_count, 3)
    counts = [array_count] + [common + (i < remainder) for i in range(3)]
    offset = 0
    plan = []
    for engine, count in zip(ENGINES, counts):
        plan.append({"engine": engine[0], "start": offset, "count": count})
        offset += count
    if offset != total:
        raise RuntimeError("invalid engine plan")
    return plan


def read_memory(pid):
    values = {}
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        name, _, content = line.partition(":")
        if name in ("VmSize", "VmRSS", "VmSwap"):
            values[name] = int(content.split()[0])
    if "VmSize" not in values or "VmRSS" not in values:
        raise RuntimeError(f"process {pid} has no current memory metrics")
    return values


def mem_available_kib():
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1])
    raise RuntimeError("MemAvailable is unavailable")


def cgroup_headroom_bytes():
    """Account for a container limit when available; otherwise return None."""
    for maximum, current in (
        ("/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory.current"),
        ("/sys/fs/cgroup/memory/memory.limit_in_bytes",
         "/sys/fs/cgroup/memory/memory.usage_in_bytes"),
    ):
        try:
            limit = Path(maximum).read_text().strip()
            if limit != "max":
                return max(0, int(limit) - int(Path(current).read_text()))
        except (OSError, ValueError):
            pass
    return None


def check_host_memory(args):
    required = args.min_available_mib * 1024
    if mem_available_kib() < required:
        raise RuntimeError("host MemAvailable is below the configured reserve")
    headroom = cgroup_headroom_bytes()
    if headroom is not None and headroom < required * 1024:
        raise RuntimeError("cgroup memory headroom is below the configured reserve")


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def clean_environment(mode, library=None, diagnostic=False):
    environment = os.environ.copy()
    for name in CLEAR_ENV:
        environment.pop(name, None)
    if mode == "jemalloc":
        environment["LD_PRELOAD"] = str(library)
    if diagnostic:
        environment["LD_DEBUG"] = "bindings"
    return environment


def capture(command):
    try:
        result = subprocess.run(
            command, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=15,
            env=clean_environment("system"),
        )
        return {"exit": result.returncode, "output": result.stdout.strip()}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"error": str(error)}


def find_jemalloc(explicit):
    if explicit:
        path = Path(explicit).resolve()
        if not path.is_file():
            raise RuntimeError(f"jemalloc library not found: {path}")
        return path
    output = capture(["ldconfig", "-p"]).get("output", "")
    for line in output.splitlines():
        if line.strip().startswith("libjemalloc.so.2 ") and " => " in line:
            path = Path(line.split(" => ", 1)[1]).resolve()
            if path.is_file():
                return path
    raise RuntimeError("jemalloc not found; provide --jemalloc-lib PATH")


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def choose_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Client:
    """One persistent connection, one outstanding command; no pipeline tuning."""

    def __init__(self, port, deadline, timeout):
        self.deadline = deadline
        self.timeout = timeout
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def close(self):
        try:
            self.file.close()
        finally:
            self.sock.close()

    def command(self, *fields):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("run deadline exceeded")
        self.sock.settimeout(min(self.timeout, remaining))
        self.sock.sendall(encode_command(*fields))
        return self.read_reply()

    def read_reply(self):
        line = self.file.readline(4096)
        if not line.endswith(b"\r\n"):
            raise RuntimeError(f"incomplete or oversized RESP header: {line[:100]!r}")
        prefix, payload = line[:1], line[1:-2]
        if prefix == b"+":
            return payload
        if prefix == b"-":
            raise RuntimeError(f"server error: {payload[:200]!r}")
        if prefix == b":":
            return int(payload)
        if prefix == b"$":
            length = int(payload)
            if length == -1:
                return None
            if not 0 <= length <= 1024 * 1024:
                raise RuntimeError(f"unexpected bulk length: {length}")
            data = self.file.read(length + 2)
            if len(data) != length + 2 or not data.endswith(b"\r\n"):
                raise RuntimeError("incomplete RESP bulk reply")
            return data[:-2]
        raise RuntimeError(f"unsupported RESP reply: {line[:100]!r}")


class Service:
    def __init__(self, args, mode, directory, library, diagnostic=False):
        self.args = args
        self.mode = mode
        self.directory = directory
        self.library = library
        self.diagnostic = diagnostic
        self.process = None
        self.client = None
        self.log = None

    def __enter__(self):
        self.directory.mkdir(parents=True)
        port = choose_port()
        backend = "pool" if self.mode == "pool" else "system"
        config = self.directory / "server.conf"
        config.write_text(
            "role=standalone\nbind_ip=127.0.0.1\n"
            f"service_port={port}\nlog_level=info\n"
            f"persistence_mode=snapshot\nallocator={backend}\n",
            encoding="utf-8",
        )

        limit = self.args.as_limit_mib * 1024 * 1024

        def child_limits():
            # Same address-space limit for all three modes; no forced trimming.
            _, hard = resource.getrlimit(resource.RLIMIT_AS)
            if hard != resource.RLIM_INFINITY and hard < limit:
                raise RuntimeError("inherited RLIMIT_AS is below requested limit")
            resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

        try:
            self.log = (self.directory / "server.log").open("wb")
            self.process = subprocess.Popen(
                [str(self.args.server), str(config.resolve())],
                cwd=self.directory,
                env=clean_environment(self.mode, self.library, self.diagnostic),
                stdin=subprocess.DEVNULL, stdout=self.log,
                stderr=subprocess.STDOUT, preexec_fn=child_limits,
            )
            self.deadline = time.monotonic() + self.args.timeout
            startup_deadline = min(self.deadline, time.monotonic() + 15)
            while time.monotonic() < startup_deadline:
                if self.process.poll() is not None:
                    raise RuntimeError(f"server startup exit={self.process.returncode}")
                try:
                    self.client = Client(port, self.deadline, self.args.socket_timeout)
                    break
                except (ConnectionError, socket.timeout):
                    time.sleep(0.05)
            if self.client is None:
                raise TimeoutError("server startup timed out")

            # A common readiness request, with no test data inserted.
            if self.client.command(b"GET", b"__memory_benchmark_ready__") is not None:
                raise RuntimeError("isolated service unexpectedly contains data")
            time.sleep(self.args.settle)
            self.guard()
            text = (self.directory / "server.log").read_text(errors="replace")
            if f"allocator initialized: {backend}" not in text:
                raise RuntimeError("startup allocator marker missing or incorrect")
            maps = Path(f"/proc/{self.process.pid}/maps").read_text()
            (self.directory / "startup-maps.txt").write_text(maps)
            if self.mode == "jemalloc":
                paths = []
                for line in maps.splitlines():
                    fields = line.split(None, 5)
                    if len(fields) == 6 and fields[5].startswith("/"):
                        paths.append(Path(fields[5].strip()).resolve())
                if self.library not in paths:
                    raise RuntimeError("requested jemalloc is not mapped into service")
            elif "jemalloc" in maps:
                raise RuntimeError("jemalloc unexpectedly loaded in system/pool run")
            return self
        except BaseException:
            self.close()
            raise

    def guard(self):
        if self.process.poll() is not None:
            raise RuntimeError(f"server exited: {self.process.returncode}")
        if time.monotonic() >= self.deadline:
            raise TimeoutError("run deadline exceeded")
        memory = read_memory(self.process.pid)
        if memory["VmRSS"] > self.args.rss_limit_mib * 1024:
            raise RuntimeError("RSS limit exceeded; run aborted")
        check_host_memory(self.args)
        return memory

    def sample(self):
        time.sleep(self.args.settle)
        raw = []
        for _ in range(5):
            raw.append(self.guard())
            time.sleep(0.05)
        return {
            "VmSize_KiB": statistics.median(x["VmSize"] for x in raw),
            "VmRSS_KiB": statistics.median(x["VmRSS"] for x in raw),
            "raw_proc_samples_KiB": raw,
            "allocator_stats": None,
            "allocator_stats_note": "not exposed by current service protocol",
        }

    def close(self):
        if self.client is not None:
            self.client.close()
            self.client = None
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.log is not None:
            self.log.close()
            self.log = None

    def __exit__(self, *_):
        self.close()


def verify_backend(args, mode, directory, library):
    """Sacrificial startup: loader diagnostics never enter measured process."""
    with Service(args, mode, directory, library, diagnostic=True):
        text = (directory / "server.log").read_text(errors="replace")
        pattern = re.compile(
            r"binding file (.+?) \[\d+\] to (.+?) \[\d+\]: "
            r"normal symbol [`'](malloc|free)'"
        )
        records = []
        for line in text.splitlines():
            match = pattern.search(line)
            if match and Path(match[1]).resolve() == args.server:
                records.append({"symbol": match[3], "provider": match[2], "line": line.strip()})
        required = ("free",) if mode == "pool" else ("malloc", "free")
        for symbol in required:
            matching = [r for r in records if r["symbol"] == symbol]
            if not matching:
                raise RuntimeError(f"cannot verify service binding: {symbol}")
            for record in matching:
                provider = Path(record["provider"]).resolve()
                valid = provider == library if mode == "jemalloc" else "libc.so" in provider.name
                if not valid:
                    raise RuntimeError(f"unexpected {symbol} provider: {provider}")
    result = {"mode": mode, "bindings": records, "diagnostic_process_excluded": True}
    write_json(directory / "verification.json", result)
    print(f"[PASS] actual service backend verification: {mode}", flush=True)
    return result


def run_phase(service, plan, phase, result, path):
    started = time.monotonic()
    stats = {"attempted": 0, "success": 0, "failed": 0, "elapsed_seconds": None}
    result["phases"][phase] = stats
    total = sum(item["count"] for item in plan)
    report_every = max(1, total // 10)
    try:
        for item, engine in zip(plan, ENGINES):
            _, set_cmd, get_cmd, del_cmd = engine
            for index in range(item["start"], item["start"] + item["count"]):
                if stats["success"] % 2048 == 0:
                    service.guard()
                key, value = make_record(index)
                stats["attempted"] += 1
                try:
                    if phase == "insert":
                        actual = service.client.command(set_cmd, key, value)
                        valid = actual == b"OK"
                    elif phase == "verify_present":
                        actual = service.client.command(get_cmd, key)
                        valid = actual == value
                    elif phase == "delete":
                        actual = service.client.command(del_cmd, key)
                        valid = actual == 1
                    else:
                        actual = service.client.command(get_cmd, key)
                        valid = actual is None
                    if not valid:
                        raise RuntimeError(
                            f"{phase}: {item['engine']} index={index}, unexpected reply {actual!r}"
                        )
                except Exception:
                    stats["failed"] += 1
                    raise
                stats["success"] += 1
                if stats["success"] % report_every == 0:
                    print(f"  {phase}: {stats['success']}/{total}", flush=True)
        if stats["success"] != total:
            raise RuntimeError("phase did not verify the requested record count")
    finally:
        stats["elapsed_seconds"] = time.monotonic() - started
        write_json(path, result)


def perform_run(args, mode, round_number, directory, library, plan, binary_hash):
    directory.mkdir()
    path = directory / "result.json"
    result = {
        "mode": mode, "round": round_number, "entries": args.entries,
        "plan": plan, "status": "running", "states": {}, "phases": {},
        "binary_sha256": binary_hash,
    }
    write_json(path, result)
    try:
        if sha256_file(args.server) != binary_hash:
            raise RuntimeError("server binary changed during experiment")
        if shutil.disk_usage(directory).free < 1024 ** 3:
            raise RuntimeError("less than 1 GiB free disk; refusing run")
        check_host_memory(args)
        result["background_processes"] = capture(
            ["ps", "-eo", "pid,comm,rss,pcpu", "--sort=-rss"]
        )
        result["verification"] = verify_backend(
            args, mode, directory / "verification", library
        )
        with Service(args, mode, directory / "measurement", library) as service:
            result["pid"] = service.process.pid
            result["states"]["start"] = service.sample()
            write_json(path, result)

            run_phase(service, plan, "insert", result, path)
            run_phase(service, plan, "verify_present", result, path)
            result["states"]["peak"] = service.sample()
            write_json(path, result)

            run_phase(service, plan, "delete", result, path)
            run_phase(service, plan, "verify_absent", result, path)
            result["states"]["end"] = service.sample()

            for filename in ("snapshot.bin", "snapshot.db", "commands.aof", "appendonly.aof"):
                if (service.directory / filename).exists():
                    raise RuntimeError(f"unexpected persistence output: {filename}")
            if sha256_file(args.server) != binary_hash:
                raise RuntimeError("server binary changed during experiment")
            result["states_measured_before_server_exit"] = True
        result["status"] = "passed"
        result["stop_method"] = "SIGTERM, SIGKILL only if termination times out"
        result["shutdown_note"] = "process termination does not prove graceful allocator cleanup"
    except BaseException as error:
        result["status"] = "failed"
        result["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        write_json(path, result)
    return result


def save_tables(output, results):
    fields = ["round", "mode", "entries", "state", "VmSize_KiB", "VmRSS_KiB",
              "insert_success", "insert_failed", "insert_seconds",
              "delete_success", "delete_failed", "delete_seconds"]
    with (output / "raw.csv").open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=fields)
        writer.writeheader()
        for result in results:
            for state, memory in result["states"].items():
                ins = result["phases"].get("insert", {})
                delete = result["phases"].get("delete", {})
                writer.writerow({
                    "round": result["round"], "mode": result["mode"],
                    "entries": result["entries"], "state": state,
                    "VmSize_KiB": memory["VmSize_KiB"], "VmRSS_KiB": memory["VmRSS_KiB"],
                    "insert_success": ins.get("success"), "insert_failed": ins.get("failed"),
                    "insert_seconds": ins.get("elapsed_seconds"),
                    "delete_success": delete.get("success"), "delete_failed": delete.get("failed"),
                    "delete_seconds": delete.get("elapsed_seconds"),
                })
    summary = []
    for mode in MODES:
        group = [r for r in results if r["mode"] == mode and r["status"] == "passed"]
        for state in ("start", "peak", "end"):
            if group:
                summary.append({
                    "mode": mode, "state": state, "rounds": len(group),
                    "VmSize_KiB": statistics.median(r["states"][state]["VmSize_KiB"] for r in group),
                    "VmRSS_KiB": statistics.median(r["states"][state]["VmRSS_KiB"] for r in group),
                })
    write_json(output / "summary.json", summary)
    lines = ["| mode | state | rounds | VmSize KiB | VmRSS KiB |",
             "|---|---|---:|---:|---:|"]
    for row in summary:
        lines.append(f"| {row['mode']} | {row['state']} | {row['rounds']} | "
                     f"{row['VmSize_KiB']} | {row['VmRSS_KiB']} |")
    (output / "summary.md").write_text("\n".join(lines) + "\n")
    print("\n" + "\n".join(lines), flush=True)


def self_test():
    assert encode_command(b"HSET", b"k", b"v") == b"*3\r\n$4\r\nHSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
    seen = set()
    for index in range(10_000):
        key, value = make_record(index)
        assert len(key) == 32 and len(value) == 128
        assert b"\0" not in key + value and b"\n" not in key + value
        assert key not in seen
        seen.add(key)
    assert make_record(123) == make_record(123)
    for total in (4, 17, 10_000, 1_000_000):
        plan = make_plan(total, 1024)
        assert sum(x["count"] for x in plan) == total
        assert plan[0]["count"] <= 1024
        for left, right in zip(plan, plan[1:]):
            assert left["start"] + left["count"] == right["start"]
    assert [x["count"] for x in make_plan(1_000_000, 1024)] == [1024, 332992, 332992, 332992]
    left, right = socket.socketpair()
    client = Client.__new__(Client)
    client.sock, client.file = left, left.makefile("rb")
    try:
        right.sendall(b"+OK\r\n:1\r\n$3\r\na\nb\r\n$-1\r\n")
        assert [client.read_reply() for _ in range(4)] == [b"OK", 1, b"a\nb", None]
        right.sendall(b"-ERR injected\r\n")
        try:
            client.read_reply()
        except RuntimeError:
            pass
        else:
            raise AssertionError("server error was accepted")
    finally:
        client.close()
        right.close()
    print("[PASS] dataset, plans, RESP encoding and reply parsing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("all",) + MODES, default="all")
    parser.add_argument("--entries", type=int, default=10_000)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--server", type=Path, default=ROOT / "bin" / "kvstore")
    parser.add_argument("--jemalloc-lib")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=3600, help="seconds per service process")
    parser.add_argument("--socket-timeout", type=float, default=15)
    parser.add_argument("--settle", type=float, default=0.2)
    parser.add_argument("--as-limit-mib", type=int, default=2048)
    parser.add_argument("--rss-limit-mib", type=int, default=1024)
    parser.add_argument("--min-available-mib", type=int, default=1024)
    parser.add_argument("--build-note", default="not supplied; exact build flags not verified")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not sys.platform.startswith("linux"):
        parser.error("measurements require Linux /proc")
    if not 4 <= args.entries <= 1_000_000 or args.rounds < 1:
        parser.error("entries must be 4..1000000 and rounds must be positive")
    if min(args.timeout, args.socket_timeout, args.as_limit_mib,
           args.rss_limit_mib, args.min_available_mib) <= 0 or args.settle < 0:
        parser.error("limits must be positive and settle must be nonnegative")
    args.server = args.server.resolve()
    if not args.server.is_file() or not os.access(args.server, os.X_OK):
        parser.error(f"executable server not found: {args.server}")
    modes = list(MODES) if args.backend == "all" else [args.backend]
    library = find_jemalloc(args.jemalloc_lib) if "jemalloc" in modes else None
    dependency = capture(["ldd", str(args.server)])
    if dependency.get("exit") != 0:
        raise RuntimeError(f"cannot inspect server dependencies: {dependency}")
    if any(name in dependency["output"] for name in ("libjemalloc", "libasan", "libubsan")):
        raise RuntimeError("use a normal binary without linked jemalloc or sanitizers")
    header = args.server.parent.parent / "include" / "kvstore.h"
    text = header.read_text()
    match = re.search(r"^\s*#define\s+KVS_ARRAY_SIZE\s+(\d+)\b", text, re.M)
    if not match:
        raise RuntimeError("cannot verify Array capacity from repository header")
    plan = make_plan(args.entries, int(match[1]))
    binary_hash = sha256_file(args.server)
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="91kv-memory-results-"))
    print(f"Results retained at: {output}", flush=True)
    print("PILOT; NOT million-record acceptance" if args.entries != 1_000_000
          else "Million-record experiment; completion still requires every validation", flush=True)
    print("Engine plan:", plan, flush=True)
    metadata = {
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "arguments": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
        "server_sha256": binary_hash, "jemalloc_library": str(library) if library else None,
        "jemalloc_sha256": sha256_file(library) if library else None,
        "dependencies": dependency, "kernel": list(platform.uname()),
        "cpu_count": os.cpu_count(), "pointer_bytes": struct.calcsize("P"),
        "lscpu": capture(["lscpu"]), "meminfo": Path("/proc/meminfo").read_text(),
        "git_head": capture(["git", "-C", str(args.server.parent.parent), "rev-parse", "HEAD"]),
        "git_status": capture(["git", "-C", str(args.server.parent.parent), "status", "--short"]),
        "compiler": capture(["gcc", "--version"]),
        "makefile_sha256": sha256_file(args.server.parent.parent / "Makefile"),
        "cleared_child_environment": {k: os.environ[k] for k in CLEAR_ENV if k in os.environ},
        "plan": plan, "key_bytes": 32, "value_bytes": 128,
        "dataset": "unique 8-byte hex id + 24 bytes SHA256/base85; hex digest repeated for value",
        "sampling": "five /proc/status samples per state; within-state median; all raw samples retained",
        "peak_definition": "all requested records inserted and GET-verified; not historical RSS maximum",
        "request_window": 1, "timing_note": "includes generation and serial network round trips; not throughput benchmark",
        "limitations": [
            "jemalloc LD_PRELOAD replaces ordinary allocations process-wide, including third-party libraries",
            "pool internal statistics are not exported by the service protocol",
            "no reinsert/reuse phase or leak conclusion from RSS alone",
            "SIGTERM cleanup is not evidence of graceful allocator destruction",
            "startup chooses a free port then releases reservation; binding collision fails the run",
            "VmRSS from /proc/status is an approximate resident-memory accounting metric",
        ],
    }
    write_json(output / "metadata.json", metadata)
    results = []
    try:
        for round_number in range(1, args.rounds + 1):
            # Rotate order to reduce a fixed mode/order bias across rounds.
            offset = (round_number - 1) % len(modes)
            order = modes[offset:] + modes[:offset]
            for mode in order:
                print(f"\nRound {round_number}/{args.rounds}: {mode}", flush=True)
                directory = output / f"round-{round_number:02d}-{mode}"
                try:
                    result = perform_run(args, mode, round_number, directory, library, plan, binary_hash)
                except BaseException:
                    path = directory / "result.json"
                    if path.exists():
                        results.append(json.loads(path.read_text()))
                    raise
                results.append(result)
                for state, memory in result["states"].items():
                    print(f"  {state}: VmSize={memory['VmSize_KiB']} KiB, "
                          f"VmRSS={memory['VmRSS_KiB']} KiB", flush=True)
    finally:
        save_tables(output, results)
        complete = (args.entries == 1_000_000 and args.rounds >= 3
                    and set(modes) == set(MODES) and len(results) == 3 * args.rounds
                    and all(r["status"] == "passed" for r in results))
        write_json(output / "completion.json", {
            "million_three_mode_multi_round_measurement_completed": complete,
            "completed_runs": sum(r["status"] == "passed" for r in results),
            "expected_runs": len(modes) * args.rounds,
            "note": "memory table completion only; not full stage-8 acceptance",
        })
        print(f"\nArtifacts retained: {output}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("Interrupted; owned service processes stopped, partial results retained", file=sys.stderr)
        sys.exit(130)
    except Exception as error:
        print(f"[FAIL] {type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(1)