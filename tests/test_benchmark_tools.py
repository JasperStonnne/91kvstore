#!/usr/bin/env python3
"""Correctness smoke for benchmark fixtures using small, owned workloads."""
import argparse
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import zlib

from test_aof_restart import encode_command
from test_persistence_100k import Client, wait_ready
from test_standalone import choose_port, stop_server


def checked(arguments, log):
    result = subprocess.run(list(map(str, arguments)), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=30)
    log.write_bytes(result.stdout)
    assert result.returncode == 0, f"exit={result.returncode}: {log}\n{result.stdout.decode(errors='replace')}"
    return result.stdout.decode()


def wait_log(process, path, marker):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        assert process.poll() is None, path.read_text(errors="replace")
        if marker in path.read_text(errors="replace"):
            return
        time.sleep(0.02)
    raise TimeoutError(str(path))


def echo(repo, root, benchmark):
    port = choose_port()
    log_path = root / "echo.log"
    with log_path.open("wb") as log:
        process = subprocess.Popen([str(repo / "bin/benchmark_echo"), "127.0.0.1", str(port)],
                                   stdout=log, stderr=log)
    try:
        wait_ready(process, port)
        with Client(port) as client:
            for value in (b"", b"hello", b"a\0b\r\n", b"x" * 128):
                assert client.command(b"ECHO", value) == value
        with socket.create_connection(("127.0.0.1", port), timeout=3) as connection:
            connection.settimeout(3)
            frame = encode_command(b"ECHO", b"first")
            connection.sendall(frame[:7])
            time.sleep(0.01)
            connection.sendall(frame[7:] + encode_command(b"ECHO", b"second"))
            with connection.makefile("rb") as reader:
                for value in (b"first", b"second"):
                    assert reader.readline() == b"$" + str(len(value)).encode() + b"\r\n"
                    assert reader.read(len(value) + 2) == value + b"\r\n"
        for depth in (1, 10, 20, 40, 80, 160):
            text = checked([benchmark, "-h", "127.0.0.1", "-p", port, "-c", 1,
                            "-P", depth, "-n", 320, "ECHO", "x" * 128],
                           root / f"echo-client-P{depth}.log")
            assert "320 requests completed" in text and "Error from server" not in text
        print("[PASS] ECHO exact payload, binary/empty values, split input, pipeline and redis-benchmark", flush=True)
    finally:
        stop_server(process)


def transfer(repo, root, host):
    def begin(mode, label):
        with socket.socket() as reservation:
            reservation.bind((host, 0))
            port = reservation.getsockname()[1]
        log_path = root / f"{label}-receive.log"
        with log_path.open("wb") as log:
            process = subprocess.Popen([str(repo / "bin/benchmark_transfer"), "receive", mode, host, str(port)],
                                       stdout=log, stderr=log)
        try:
            wait_log(process, log_path, "READY")
        except BaseException:
            stop_server(process)
            raise
        return process, port, log_path

    for mode in ("sendfile", "rdma"):
        for length in (0, 1024 * 1024, 1024 * 1024 + 17):
            path = root / "transfer-input"
            data = (bytes(range(256)) * (length // 256 + 1))[:length]
            path.write_bytes(data)
            label = f"{mode}-{length}"
            process, port, log = begin(mode, label)
            try:
                text = checked([repo / "bin/benchmark_transfer", "send", mode, host, port, path],
                               root / f"{label}-send.log")
                assert process.wait(timeout=10) == 0, log.read_text()
                sent = json.loads(text.splitlines()[-1])
                received = json.loads(log.read_text().splitlines()[-1])
                for result in (sent, received):
                    assert result["verified"] and result["bytes"] == length
                    assert result["crc32"] == f"{zlib.crc32(data):08x}"
                    assert result["chunk_bytes"] == 1048576 and result["window"] == 1
                print(f"[PASS] {mode} {length} bytes, exact CRC and bounded cleanup", flush=True)
            finally:
                stop_server(process)
    process, port, log = begin("sendfile", "bad-crc")
    try:
        with socket.create_connection((host, port), timeout=3) as connection:
            connection.settimeout(3)
            connection.sendall(struct.pack("!QII", 3, 0, 0))
            assert connection.recv(1) == b"R"
            connection.sendall(b"bad")
            assert connection.recv(1) == b"A"
            assert connection.recv(1) == b"F"
        assert process.wait(timeout=5) == 1
        assert not json.loads(log.read_text().splitlines()[-1])["verified"]
        print("[PASS] transfer rejects mismatched CRC", flush=True)
    finally:
        stop_server(process)


def redis(root, server, benchmark):
    for mode in ("no", "yes"):
        directory = root / f"redis-{mode}"
        directory.mkdir()
        port = choose_port()
        config = directory / "redis.conf"
        config.write_text(f'bind 127.0.0.1\nport {port}\nsave ""\nappendonly {mode}\n'
                          'appendfsync always\nauto-aof-rewrite-percentage 0\nio-threads 1\n')
        with (directory / "server.log").open("wb") as log:
            process = subprocess.Popen([str(server), str(config)], cwd=directory, stdout=log, stderr=log)
        try:
            wait_ready(process, port)
            with Client(port) as client:
                assert client.command(b"SET", b"key", b"value") == b"OK"
                assert client.command(b"GET", b"key") == b"value"
            cli = server.parent / "redis-cli"
            for name, expected in (("appendonly", mode), ("appendfsync", "always"),
                                   ("auto-aof-rewrite-percentage", "0"), ("io-threads", "1")):
                text = checked([cli, "--raw", "-p", port, "CONFIG", "GET", name], directory / f"{name}.log")
                assert text.splitlines() == [name, expected], text
            text = checked([benchmark, "-p", port, "-c", 1, "-P", 1, "-n", 200,
                            "SET", "key", "x" * 128], directory / "client.log")
            assert "200 requests completed" in text and "Error from server" not in text
        finally:
            stop_server(process)
        print(f"[PASS] Redis 7.0.15 appendonly={mode}, SET/GET and startup configuration", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path)
    parser.add_argument("--rdma-host", required=True)
    parser.add_argument("--redis-server", type=Path, required=True)
    parser.add_argument("--redis-benchmark", type=Path, required=True)
    args = parser.parse_args()
    repo = args.repo.resolve()
    root = Path(tempfile.mkdtemp(prefix="91kv-tool-smoke-"))
    print(f"Evidence: {root}", flush=True)
    echo(repo, root, args.redis_benchmark)
    transfer(repo, root, args.rdma_host)
    redis(root, args.redis_server, args.redis_benchmark)
    checked(["python3", "-B", repo / "tests/benchmark_save_frequency.py", "--server", repo / "bin/kvstore",
             "--entries", 32, "--save-every", 7, 8, "--rounds", 1, "--output", root / "save"], root / "save-driver.log")
    summary = json.loads((root / "save/summary.json").read_text())
    assert summary["passed"] and len(summary["results"]) == 2
    assert all(item["verified"] == 32 for item in summary["results"])
    print("[PASS] SAVE driver exact checkpoint count, final partial interval and restart verification", flush=True)


if __name__ == "__main__":
    main()
