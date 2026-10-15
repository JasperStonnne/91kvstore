#!/usr/bin/env python3
"""Snapshot / AOF 各 100000 条的独立持久化验收。"""

import hashlib
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from test_snapshot_restart import launch
from test_standalone import choose_port, stop_server


ENGINE_CASES = (
    ("Array", b"", 1000),
    ("RBTree", b"R", 33000),
    ("Hash", b"H", 33000),
    ("Skiplist", b"S", 33000),
)
TOTAL = sum(count for _, _, count in ENGINE_CASES)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def record(prefix, index):
    """每次运行都生成相同的 key/value，包括空格和 CRLF。"""
    key = b"accept_" + prefix + b"_" + str(index).encode("ascii")
    value = b"value " + key + b"\r\nsecond line"
    return key, value


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.sock.settimeout(120)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.reader = self.sock.makefile("rb")

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.reader.close()
        self.sock.close()

    def command(self, *fields):
        request = b"*" + str(len(fields)).encode("ascii") + b"\r\n"
        for field in fields:
            request += (
                b"$" + str(len(field)).encode("ascii") + b"\r\n"
                + field + b"\r\n"
            )
        self.sock.sendall(request)

        line = self.reader.readline()
        require(line.endswith(b"\r\n"), f"回复不完整：{line!r}")

        if line.startswith(b"+"):
            return line[1:-2]
        if line.startswith(b"-"):
            raise AssertionError(f"命令 {fields!r} 失败：{line!r}")
        if line.startswith(b":"):
            return int(line[1:-2])
        if line.startswith(b"$"):
            length = int(line[1:-2])
            if length == -1:
                return None
            require(length >= 0, f"非法 Bulk 长度：{length}")
            body = self.reader.read(length + 2)
            require(
                len(body) == length + 2 and body.endswith(b"\r\n"),
                "Bulk 回复不完整",
            )
            return body[:-2]

        raise AssertionError(f"不支持的回复：{line!r}")


def wait_ready(process, port, timeout=300):
    """为 10 万条恢复预留时间；超时明确失败。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        require(
            process.poll() is None,
            f"服务启动前退出，exit={process.returncode}",
        )
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                require(process.poll() is None, "服务连接成功后立即退出")
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"服务在 {timeout} 秒内未就绪")


def stop_checked(process):
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired as exc:
        raise AssertionError("SIGTERM 后 10 秒内未退出") from exc
    require(
        process.returncode in (0, -15),
        f"停服退出码异常：{process.returncode}",
    )


def file_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_mode(mode):
    directory = Path(tempfile.mkdtemp(prefix=f"91kv-{mode}-100k-"))
    port = choose_port()
    process = None
    print(f"\n[{mode}] 文件和日志：{directory}", flush=True)

    (directory / "server.conf").write_text(
        "role=standalone\n"
        "bind_ip=127.0.0.1\n"
        f"service_port={port}\n"
        "log_level=error\n"
        f"persistence_mode={mode}\n",
        encoding="utf-8",
    )

    try:
        process = launch(directory)
        wait_ready(process, port)

        started = time.monotonic()
        written = 0
        save_seconds = None

        with Client(port) as client:
            for name, prefix, count in ENGINE_CASES:
                for index in range(count):
                    key, value = record(prefix, index)
                    reply = client.command(prefix + b"SET", key, value)
                    require(reply == b"OK", f"{name} 写入失败：{key!r}")
                    written += 1
                    if written % 10000 == 0:
                        print(f"[{mode}] 已写入 {written}/{TOTAL}", flush=True)
                print(f"[{mode}] {name} 写入 {count} 条成功", flush=True)

            write_seconds = time.monotonic() - started
            require(written == TOTAL == 100000, "写入总数错误")

            if mode == "snapshot":
                started = time.monotonic()
                require(client.command(b"SAVE") == b"OK", "SAVE 未成功")
                save_seconds = time.monotonic() - started

        # AOF 分支没有发送 SAVE。
        stop_checked(process)
        process = None

        filename = "snapshot.bin" if mode == "snapshot" else "commands.aof"
        data_path = directory / filename
        require(data_path.is_file(), f"未生成 {filename}")
        require(data_path.stat().st_size > 0, f"{filename} 为空")

        forbidden = ["snapshot.db", "appendonly.aof"]
        forbidden += ["commands.aof"] if mode == "snapshot" else ["snapshot.bin"]
        for name in forbidden:
            require(not (directory / name).exists(), f"意外生成文件：{name}")

        before_digest = file_digest(data_path)

        started = time.monotonic()
        process = launch(directory)
        wait_ready(process, port)
        restart_seconds = time.monotonic() - started

        started = time.monotonic()
        verified = 0
        with Client(port) as client:
            for name, prefix, count in ENGINE_CASES:
                for index in range(count):
                    key, expected = record(prefix, index)
                    actual = client.command(prefix + b"GET", key)
                    require(
                        actual == expected,
                        f"{name} {key!r} 不一致："
                        f"expected={expected!r}, actual={actual!r}",
                    )
                    verified += 1
                    if verified % 10000 == 0:
                        print(f"[{mode}] 已核对 {verified}/{TOTAL}", flush=True)
                print(f"[{mode}] {name} 逐条核对 {count} 条成功", flush=True)

        verify_seconds = time.monotonic() - started
        require(verified == TOTAL, "读回校验总数错误")
        require(file_digest(data_path) == before_digest, "恢复或读取修改了持久化文件")

        stop_checked(process)
        process = None
        require(file_digest(data_path) == before_digest, "第二次停服修改了文件")

        print(
            f"[PASS] {mode}: 写入={written}, 读回一致={verified}\n"
            f"  写入耗时={write_seconds:.3f}s\n"
            f"  重启至端口就绪={restart_seconds:.3f}s\n"
            f"  逐条核对耗时={verify_seconds:.3f}s\n"
            f"  文件大小={data_path.stat().st_size} bytes",
            flush=True,
        )
        if save_seconds is not None:
            print(f"  SAVE 耗时={save_seconds:.3f}s", flush=True)
        return True

    except Exception as exc:
        print(f"[FAIL] {mode}: {exc!r}", flush=True)
        print(f"文件及 server.log 保留在：{directory}", flush=True)
        return False
    finally:
        # 仅用于失败后的清理；不能把强制清理当成验收成功。
        stop_server(process)


def main():
    results = [run_mode(mode) for mode in ("snapshot", "aof")]
    passed = sum(results)
    print(f"\n100000 条持久化验收：{passed}/2 模式通过", flush=True)
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())