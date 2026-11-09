#!/usr/bin/env python3
"""复用现有单测，在临时目录重编译 ASan/UBSan，不复用普通 build 对象。"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

TESTS = ("test_protocol", "test_config", "test_snapshot_store",
         "test_hash_memory_pool", "test_rbtree_memory_pool", "test_skiplist_memory_pool",
         "test_array_memory", "test_allocator", "test_buffer_allocator",
         "test_mapped_file", "test_uring_io", "test_aof_replay",
         "test_connection_protocol", "test_replication_boundary", "test_ebpf_online")


def main():
    if not shutil.which("gcc") or not shutil.which("make"):
        print("[SKIP] gcc or make unavailable")
        return 77
    repo = Path(__file__).resolve().parent.parent
    root = Path(tempfile.mkdtemp(prefix="91kv-sanitizers-"))
    objects, binaries = root / "objects", root / "bin"
    objects.mkdir()
    binaries.mkdir()
    # 配置单测使用相对路径；只复制三个版本化示例，运行仍留在临时目录。
    examples = root / "config"
    examples.mkdir()
    for name in ("standalone.conf", "primary.conf", "replica.conf"):
        shutil.copyfile(repo / "config" / name, examples / name)
    flags = ("-std=gnu11 -Wall -Wextra -O1 -g -fno-omit-frame-pointer "
             "-fsanitize=address,undefined -fno-sanitize-recover=all")
    command = ["make", "-j2", f"BUILD_DIR={objects}", f"BIN_DIR={binaries}",
               f"CFLAGS={flags}", *[str(binaries / name) for name in TESTS]]
    result = {"passed": False, "tests": [],
              "scope": "unit binaries; real NtyCo/CM/CQ/BPF service runtime not covered"}
    print(f"[INFO] sanitizer evidence: {root}", flush=True)
    try:
        with (root / "build.log").open("wb") as log:
            built = subprocess.run(command, cwd=repo, stdout=log, stderr=subprocess.STDOUT,
                                   timeout=120)
        result["build_exit"] = built.returncode
        if built.returncode:
            print("\n".join((root / "build.log").read_text(errors="replace").splitlines()[-30:]))
            return built.returncode
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        for name in TESTS:
            with (root / f"{name}.log").open("wb") as log:
                done = subprocess.run([str(binaries / name)], cwd=root,
                                      env=env, stdout=log, stderr=subprocess.STDOUT, timeout=30)
            result["tests"].append({"name": name, "exit": done.returncode})
            if done.returncode:
                print((root / f"{name}.log").read_text(errors="replace"), file=sys.stderr)
                print(f"[FAIL] sanitized {name}; repro binary and log retained", flush=True)
                return done.returncode
            print(f"[PASS] ASan/UBSan {name}", flush=True)
        result["passed"] = True
        return 0
    finally:
        (root / "build-command.json").write_text(json.dumps(command, indent=2) + "\n")
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"[FAIL] sanitizer diagnostics: {error!r}", file=sys.stderr)
        raise SystemExit(1)
