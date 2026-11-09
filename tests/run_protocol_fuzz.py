#!/usr/bin/env python3
"""隔离 clang/libFuzzer/ASan/UBSan；保存 corpus、原始崩溃输入及完整日志。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=50000)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    if not shutil.which(args.cc):
        print(f"[SKIP] compiler unavailable: {args.cc}", flush=True)
        return 77
    if args.runs < 1:
        raise ValueError("--runs must be positive")
    repo = Path(__file__).resolve().parent.parent
    root = Path(tempfile.mkdtemp(prefix="91kv-protocol-fuzz-"))
    corpus, artifacts = root / "corpus", root / "artifacts"
    corpus.mkdir()
    artifacts.mkdir()
    seeds = [
        b"*2\r\n$3\r\nGET\r\n$1\r\nk\r\n",
        b"*3\r\n$4\r\nHSET\r\n$1\r\nk\r\n$4\r\na\r\nb\r\n",
        b"*1\r\n$0\r\n\r\n", b"*01\r\n$01\r\nx\r\n",
        b"*2\r\n$3\r\nGET\r\n$999999999999999999999999999999\r\n",
        b"*128\r\n", b"*2\r\n$3\r\nGET\r\n$1\r\n", b"\x00\xffPING\r\n",
        b"PING\r\n", b"*1\r\n$1\r\n\x00\r\n",
    ]
    seeds.append(seeds[0] + seeds[0])
    for index, data in enumerate(seeds):
        (corpus / f"seed-{index:02}").write_bytes(data)
    compiler = subprocess.check_output([args.cc, "--version"], text=True)
    (root / "compiler.txt").write_text(compiler)
    binary = root / "fuzz_protocol"
    command = [args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
               "-fno-omit-frame-pointer", "-fsanitize=fuzzer,address,undefined",
               "-fno-sanitize-recover=all", "-I" + str(repo / "include"),
               str(repo / "tests/fuzz_protocol.c"), str(repo / "src/protocol/codec.c"),
               "-o", str(binary)]
    print(f"[INFO] fuzz evidence: {root}", flush=True)
    result = {"runs_requested": args.runs, "passed": False,
              "source_sha256": {str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in (repo / "tests/fuzz_protocol.c", repo / "src/protocol/codec.c",
                            repo / "include/protocol.h")}}
    try:
        with (root / "build.log").open("wb") as log:
            built = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        result["build_exit"] = built.returncode
        if built.returncode:
            build_error = (root / "build.log").read_text(errors="replace")
            print(build_error, file=sys.stderr)
            if "cannot find" in build_error and "libclang_rt." in build_error:
                result.update(status="SKIP", reason="missing clang runtime libraries")
                print("[SKIP] clang runtime libraries unavailable; fuzz was not run", flush=True)
                return 77
            result["status"] = "FAIL"
            return built.returncode
        run = [str(binary), str(corpus), f"-runs={args.runs}", "-max_len=8192",
               "-timeout=2", "-rss_limit_mb=512", f"-artifact_prefix={artifacts}/"]
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        with (root / "fuzzer.log").open("wb") as log:
            done = subprocess.run(run, env=env, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=120)
        result.update(exit=done.returncode, passed=done.returncode == 0,
                      artifacts=[p.name for p in artifacts.iterdir()])
        print("\n".join((root / "fuzzer.log").read_text(errors="replace").splitlines()[-8:]))
        print("[PASS] bounded protocol fuzz with ASan/UBSan" if done.returncode == 0
              else "[FAIL] protocol fuzz; exact reproducer retained", flush=True)
        return done.returncode
    finally:
        (root / "build-command.json").write_text(json.dumps(command, indent=2) + "\n")
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"[FAIL] protocol fuzz: {error!r}", file=sys.stderr)
        raise SystemExit(1)
