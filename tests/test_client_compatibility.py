#!/usr/bin/env python3
"""可选客户端验收：支持场景必须通过；默认 benchmark PING 单独记录边界。"""
import argparse
import json
import re
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

from test_persistence_100k import Client, require, wait_ready
from test_snapshot_restart import launch
from test_standalone import PROJECT_DIR, choose_port, stop_server


class UnsupportedScenario(RuntimeError):
    """已观察到的协议不兼容，与超时/启动/工具故障分开。"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-default-ping", action="store_true",
                        help="严格模式：默认 PING 不兼容或任何依赖缺失 SKIP 均返回失败")
    args = parser.parse_args()
    directory = Path(tempfile.mkdtemp(prefix="91kv-client-compatibility-"))
    results = []
    port = choose_port()
    (directory / "server.conf").write_text(
        f"role=standalone\nbind_ip=127.0.0.1\nservice_port={port}\n"
        "log_level=error\npersistence_mode=snapshot\n", encoding="utf-8")
    process = None

    def run(name, command, timeout=30):
        # 原始输出落在本轮目录；timeout 不会把挂住的客户端写成 PASS。
        output = directory / f"{name}.log"
        with output.open("wb") as log:
            try:
                completed = subprocess.run(command, stdin=subprocess.DEVNULL,
                                           stdout=log, stderr=subprocess.STDOUT,
                                           timeout=timeout)
                return completed.returncode, output.read_bytes().decode(errors="replace"), False
            except subprocess.TimeoutExpired:
                return None, output.read_bytes().decode(errors="replace"), True

    def scenario(name, dependency, check, boundary=False):
        if dependency is None:
            item = {"name": name, "status": "SKIP", "reason": "optional dependency missing"}
        else:
            try:
                detail = check(dependency)
                item = {"name": name, "status": "PASS", **detail}
            except Exception as exc:
                item = {"name": name, "status": "UNSUPPORTED" if boundary and isinstance(exc, UnsupportedScenario) else "FAIL",
                        "reason": str(exc)}
        results.append(item)
        print(f"[{item['status']}] {name}: {item.get('reason', 'supported scenario verified')}", flush=True)

    try:
        process = launch(directory)
        wait_ready(process, port)

        def cli_check(executable):
            key, value = "cli compatibility key", "a b\r\nc"
            cases = [("ping", ["PING"], "PONG\n"),
                     ("set", ["SET", key, value], "OK\n"),
                     ("get", ["GET", key], value + "\n"),
                     ("mod", ["MOD", key, "changed"], "OK\n"),
                     ("get-modified", ["GET", key], "changed\n"),
                     ("del", ["DEL", key], "1\n"),
                     ("missing", ["GET", key], "\n")]
            for label, fields, expected in cases:
                code, output, timeout = run("cli-" + label,
                    [executable, "-h", "127.0.0.1", "-p", str(port), "--raw", *fields])
                require(not timeout and code == 0 and output == expected,
                        f"redis-cli {label}: exit={code}, timeout={timeout}, output={output!r}")
            return {"checks": len(cases), "distinct_keys": 1}

        scenario("redis-cli-supported", shutil.which("redis-cli"), cli_check)

        def hiredis_check(tool):
            code, flags, timeout = run("hiredis-flags", [tool, "--cflags", "--libs", "hiredis"])
            require(code == 0 and not timeout, "pkg-config failed for installed hiredis")
            executable = directory / "test_hiredis"
            code, output, timeout = run("hiredis-build", [shutil.which("cc"),
                str(PROJECT_DIR / "tests/test_hiredis.c"), *shlex.split(flags), "-o", str(executable)])
            require(code == 0 and not timeout, f"hiredis compile failed: {output}")
            code, output, timeout = run("hiredis", [str(executable), str(port)])
            require(code == 0 and not timeout and output.count("[PASS]") == 8,
                    f"hiredis failed: exit={code}, timeout={timeout}, output={output!r}")
            return {"checks": 8, "distinct_keys": 1}

        pkg = shutil.which("pkg-config")
        hiredis = pkg if pkg and shutil.which("cc") and subprocess.run(
            [pkg, "--exists", "hiredis"], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL).returncode == 0 else None
        scenario("hiredis-supported", hiredis, hiredis_check)

        benchmark = shutil.which("redis-benchmark")
        base = ["-h", "127.0.0.1", "-p", str(port), "-n", "10000", "-c", "1"]

        def benchmark_check(executable, name, fields, expected_sections):
            code, output, timeout = run(name, [executable, *base, *fields], timeout=20)
            if (name == "benchmark-default-ping" and not timeout and code != 0
                    and "Error from server: ERR invalid or oversized request" in output):
                raise UnsupportedScenario(
                    f"default PING_INLINE rejected: ERR invalid or oversized request; raw output: {directory / (name + '.log')}")
            # 不能只信退出码：客户端可能在报错后仍输出吞吐数字。
            sections = len(re.findall(r"10000 requests completed", output))
            errors = re.search(r"(?i)(error|unknown command|invalid|failed|server closed)", output)
            require(not timeout and code == 0 and sections == expected_sections and not errors,
                    f"exit={code}, timeout={timeout}, completed_sections={sections}, "
                    f"error_seen={bool(errors)}; raw output: {directory / (name + '.log')}")
            return {"requests_per_section": 10000, "completed_sections": sections}

        # 默认 -t ping 会测试 PING_INLINE/PING_MBULK；与显式 RESP 命令分开报告。
        scenario("redis-benchmark-default-ping", benchmark,
                 lambda executable: benchmark_check(executable, "benchmark-default-ping",
                                                     ["-t", "ping"], 2), boundary=True)
        # 默认 PING 即使失败，也继续检查明确支持的 RESP 场景。
        scenario("redis-benchmark-resp-ping", benchmark,
                 lambda executable: benchmark_check(executable, "benchmark-resp-ping", ["PING"], 1))
        with Client(port) as client:
            require(client.command(b"HSET", b"benchmark_probe", b"known value") == b"OK", "benchmark seed failed")
        scenario("redis-benchmark-hget", benchmark,
                 lambda executable: benchmark_check(executable, "benchmark-hget",
                                                     ["HGET", "benchmark_probe"], 1))
        with Client(port) as client:
            require(client.command(b"HGET", b"benchmark_probe") == b"known value", "benchmark changed data")
    except Exception as exc:
        results.append({"name": "runner", "status": "FAIL", "reason": repr(exc)})
        print(f"[FAIL] runner: {exc!r}", flush=True)
    finally:
        stop_server(process)
        counts = {status: sum(item["status"] == status for item in results)
                  for status in ("PASS", "SKIP", "FAIL", "UNSUPPORTED")}
        # 可选入口允许 SKIP；严格会议验收必须实际运行全部场景且全部通过。
        success = counts["FAIL"] == 0 and not (
            args.require_default_ping and (counts["UNSUPPORTED"] or counts["SKIP"]))
        report = {"supported_scenarios_success": success, "strict": args.require_default_ping,
                  "counts": counts, "results": results,
                  "scope": "listed scenarios only; SKIP/UNSUPPORTED are not PASS; not a performance result"}
        (directory / "result.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"[INFO] counts={counts}; evidence: {directory}", flush=True)
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
