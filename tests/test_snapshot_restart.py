import shutil
import socket
import subprocess
import sys
import zlib
import tempfile
from pathlib import Path

from test_standalone import choose_port, stop_server, wait_for_server


PROJECT_DIR = Path(__file__).resolve().parent.parent
SERVER = PROJECT_DIR / "bin" / "kvstore"


def command(port, *fields):
    """发送 RESP2 命令并读取一个响应，不改变服务端协议。"""
    request = b"*" + str(len(fields)).encode() + b"\r\n"

    for field in fields:
        request += (
            b"$" + str(len(field)).encode() + b"\r\n"
            + field + b"\r\n"
        )

    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.settimeout(3)
        sock.sendall(request)

        with sock.makefile("rb") as reply:
            line = reply.readline()
            assert line.endswith(b"\r\n"), f"不完整响应：{line!r}"

            if line.startswith(b"+"):
                return line[1:-2]

            if line.startswith(b":"):
                return int(line[1:-2])

            if line.startswith(b"$"):
                length = int(line[1:-2])
                if length == -1:
                    return None

                assert length >= 0
                value = reply.read(length)
                assert len(value) == length
                assert reply.read(2) == b"\r\n"
                return value

            raise AssertionError(
                f"命令 {fields!r} 返回非预期响应：{line!r}"
            )


def launch(directory):
    with (directory / "server.log").open("ab") as log:
        return subprocess.Popen(
            [str(SERVER), str(directory / "server.conf")],
            cwd=directory,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
        )


def main():
    directory = Path(tempfile.mkdtemp(prefix="91kv-snapshot-restart-"))
    port = choose_port()
    process = None
    result = 1

    (directory / "server.conf").write_text(
        "role=standalone\n"
        "bind_ip=127.0.0.1\n"
        f"service_port={port}\n"
        "allocator=pool\n"
        "persistence_mode=snapshot\n",
        encoding="utf-8",
    )

    try:
        # 第一次启动：从空目录写入，但不 SAVE。
        process = launch(directory)
        wait_for_server(process, port)

        assert command(port, b"HSET", b"never_saved", b"value") == b"OK"

        stop_server(process)
        process = None

        assert not (directory / "snapshot.bin").exists()
        assert not (directory / "appendonly.aof").exists()

        # 第二次启动：未 SAVE 的数据应当不存在。
        process = launch(directory)
        wait_for_server(process, port)

        assert command(port, b"HGET", b"never_saved") is None

        prefixes = (b"", b"R", b"H", b"S")
        saved_values = {}

        for prefix in prefixes:
            value = b"saved " + prefix + b" value\r\nsecond line"
            saved_values[prefix] = value

            assert command(
                port, prefix + b"SET", b"name", b"first"
            ) == b"OK"

            assert command(
                port, prefix + b"MOD", b"name", value
            ) == b"OK"

            assert command(
                port, prefix + b"SET", b"deleted", b"temporary"
            ) == b"OK"

            assert command(port, prefix + b"DEL", b"deleted") == 1

        assert command(port, b"SAVE") == b"OK"
        assert (directory / "snapshot.bin").is_file()

        # SAVE 后再次修改，但不再保存。
        for prefix in prefixes:
            assert command(
                port, prefix + b"MOD", b"name", b"not saved"
            ) == b"OK"

        assert command(
            port, b"HSET", b"after_save", b"not saved"
        ) == b"OK"

        stop_server(process)
        process = None

        # 第三次启动：恢复最近一次成功 SAVE 的状态。
        process = launch(directory)
        wait_for_server(process, port)

        for prefix in prefixes:
            assert command(
                port, prefix + b"GET", b"name"
            ) == saved_values[prefix]

            assert command(
                port, prefix + b"GET", b"deleted"
            ) is None

        assert command(port, b"HGET", b"after_save") is None
        assert command(port, b"HGET", b"never_saved") is None

        assert not (directory / "appendonly.aof").exists()
        assert not (directory / "snapshot.db").exists()

        # 停止正常运行的服务器，再测试损坏文件启动。
        stop_server(process)
        process = None

        snapshot_path = directory / "snapshot.bin"
        original = snapshot_path.read_bytes()

        bad_crc = bytearray(original)
        bad_crc[-1] ^= 1

        damaged_cases = {
            "CRC damaged": bytes(bad_crc),
            "truncated": original[:-1],
            "empty file": b"",
            "extra byte": original + b"x",
        }
        # 先确认 Python 与服务端使用的 CRC32 算法一致。
        assert zlib.crc32(original[:-4]) == int.from_bytes(
            original[-4:], "big"
        )

        # 文件头占20字节，其中第12～19字节保存记录总数。
        record_count = int.from_bytes(original[12:20], "big")
        position = 20

        # 为快照中的每条记录分别构造一个重复记录文件。
        for index in range(record_count):
            # 每条记录：引擎类型1字节、key长度4字节、value长度4字节。
            engine = original[position]
            key_length = int.from_bytes(
                original[position + 1:position + 5], "big"
            )
            value_length = int.from_bytes(
                original[position + 5:position + 9], "big"
            )

            # 找到本条记录的结束位置，取出它的完整字节。
            end = position + 9 + key_length + value_length
            assert end <= len(original) - 4
            duplicate_record = original[position:end]

            # 更新头部记录数，保留原记录，并在末尾追加重复记录。
            payload = (
                original[:12]
                + (record_count + 1).to_bytes(8, "big")
                + original[20:-4]
                + duplicate_record
            )

            # 为修改后的内容重新计算CRC，避免因CRC错误提前被拒绝。
            checksum = zlib.crc32(payload)
            damaged_cases[
                f"duplicate record {index}, engine={engine}"
            ] = payload + checksum.to_bytes(4, "big")

            position = end

        assert position == len(original) - 4
        for name, content in damaged_cases.items():
            snapshot_path.write_bytes(content)

            log_path = directory / "server.log"
            log_offset = log_path.stat().st_size

            process = launch(directory)

            try:
                return_code = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                raise AssertionError(
                    f"{name}: 损坏快照没有让服务器及时退出"
                )

            assert return_code == 1, (
                f"{name}: 预期明确报错退出，实际退出码为 {return_code}"
            )

            new_log = log_path.read_bytes()[log_offset:]
            assert b"failed to load binary snapshot" in new_log, (
                f"{name}: 没有预期的快照加载失败日志"
            )

            process = None

        print("[PASS] Snapshot rejects corrupted files at startup")
        result = 0

    except Exception as exc:
        print(f"[FAIL] Snapshot restart: {exc!r}", file=sys.stderr)

    finally:
        stop_server(process)

    if result == 0:
        shutil.rmtree(directory)
        print("[PASS] Snapshot restart and SAVE boundary")
    else:
        print(f"测试文件及日志保留在：{directory}", file=sys.stderr)

    return result


if __name__ == "__main__":
    sys.exit(main())