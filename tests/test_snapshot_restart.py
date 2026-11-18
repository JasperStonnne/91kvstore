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


def test_record_recovery(directory, port):
    """实际 SAVE/重启覆盖逐条跳过、结构拒绝和配置切换。"""
    config_path = directory / "server.conf"
    base_config = config_path.read_text()
    config_path.write_text(base_config + "snapshot_checksum=record\n")
    snapshot_path = directory / "snapshot.bin"
    snapshot_path.unlink()
    process = None
    try:
        process = launch(directory)
        wait_for_server(process, port)
        for key in (b"first", b"middle", b"last"):
            assert command(port, b"SET", key, b"value") == b"OK"
        assert command(port, b"HSET", b"other_engine", b"kept") == b"OK"
        assert command(port, b"SAVE") == b"OK"
        stop_server(process)
        process = None
        original = snapshot_path.read_bytes()
        assert int.from_bytes(original[8:12], "big") == 3 and original[12] == 1
        counts = [int.from_bytes(original[16 + i * 8:24 + i * 8], "big") for i in range(4)]
        structure = bytearray(original[:48])
        position = 48
        records = []
        for section, count in enumerate(counts):
            for index in range(count):
                boundary = position
                body_length = int.from_bytes(original[position:position + 4], "big")
                structure.extend(original[position:position + 4])
                position += 4
                body = original[position:position + body_length]
                assert zlib.crc32(body) == int.from_bytes(
                    original[position + body_length:position + body_length + 4], "big")
                key_length = int.from_bytes(body[:4], "big")
                value_length = int.from_bytes(body[4:8], "big")
                assert 8 + key_length + value_length == body_length
                records.append((section, index, boundary, body[8:8 + key_length],
                                position + 8 + key_length, position + body_length))
                position += body_length + 4
        assert position == len(original) - 4
        assert zlib.crc32(structure) == int.from_bytes(original[-4:], "big")
        array_records = [record for record in records if record[0] == 0]
        assert len(array_records) == 3

        # 换为 file 配置也按已有文件头加载；下一次 SAVE 才使用新配置。
        config_path.write_text(base_config + "snapshot_checksum=file\n")
        for record in array_records:
            _, index, boundary, broken_key, value_offset, _ = record
            damaged = bytearray(original)
            damaged[value_offset] ^= 1
            snapshot_path.write_bytes(damaged)
            log_path = directory / "server.log"
            log_offset = log_path.stat().st_size
            process = launch(directory)
            wait_for_server(process, port)
            for key in (b"first", b"middle", b"last"):
                expected = None if key == broken_key else b"value"
                assert command(port, b"GET", key) == expected
            assert command(port, b"HGET", b"other_engine") == b"kept"
            stop_server(process)
            process = None
            log = log_path.read_bytes()[log_offset:]
            expected_log = f"skipped corrupt record section=1 record={index} offset={boundary}".encode()
            assert expected_log in log
            assert log.count(b"skipped corrupt record") == 1
            assert snapshot_path.read_bytes() == damaged, "recovery rewrote damaged file"

        for offset in (12, 13, 23, 48, len(original) - 1):
            damaged = bytearray(original)
            damaged[offset] ^= 1
            snapshot_path.write_bytes(damaged)
            process = launch(directory)
            assert process.wait(timeout=5) == 1
            process = None
            assert snapshot_path.read_bytes() == damaged

        # 完整 record 文件可恢复，再次 SAVE 按 file 配置写出 v3。
        snapshot_path.write_bytes(original)
        process = launch(directory)
        wait_for_server(process, port)
        assert command(port, b"GET", b"middle") == b"value"
        assert command(port, b"SAVE") == b"OK"
        rewritten = snapshot_path.read_bytes()
        assert rewritten[12] == 0
        assert zlib.crc32(rewritten[:-4]) == int.from_bytes(rewritten[-4:], "big")
        print("[PASS] record SAVE/restart: first/middle/last skip logged; structure rejected; mode switch")
    finally:
        stop_server(process)


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

        assert int.from_bytes(original[8:12], "big") == 3
        assert original[12:16] == b"\0" * 4  # 默认 file 模式
        counts = [int.from_bytes(original[16 + i * 8:24 + i * 8], "big") for i in range(4)]
        position = 48
        for section, count in enumerate(counts):
            for index in range(count):
                key_length = int.from_bytes(original[position:position + 4], "big")
                value_length = int.from_bytes(original[position + 4:position + 8], "big")
                end = position + 8 + key_length + value_length
                payload = bytearray(original[:-4])
                # 在所属段内部复制记录，再更新该段计数并重算整文件 CRC。
                payload[end:end] = original[position:end]
                offset = 16 + section * 8
                payload[offset:offset + 8] = (count + 1).to_bytes(8, "big")
                damaged_cases[f"duplicate record {index}, section={section}"] = (
                    payload + zlib.crc32(payload).to_bytes(4, "big"))
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
        test_record_recovery(directory, port)
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