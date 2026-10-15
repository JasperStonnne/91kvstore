import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

from test_standalone import choose_port, stop_server, wait_for_server
from test_snapshot_restart import command, launch


def encode_command(*fields):
    request = b"*" + str(len(fields)).encode() + b"\r\n"
    for field in fields:
        request += (
            b"$" + str(len(field)).encode() + b"\r\n"
            + field + b"\r\n"
        )
    return request


def expect_error(port, *fields):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.settimeout(3)
        sock.sendall(encode_command(*fields))
        with sock.makefile("rb") as replies:
            reply = replies.readline()

    assert reply.startswith(b"-ERR ") and reply.endswith(b"\r\n"), (
        f"预期错误回复，实际为 {reply!r}"
    )


def main():
    directory = Path(tempfile.mkdtemp(prefix="91kv-aof-restart-"))
    port = choose_port()
    process = None
    result = 1
    aof_path = directory / "commands.aof"

    (directory / "server.conf").write_text(
        "role=standalone\n"
        "bind_ip=127.0.0.1\n"
        f"service_port={port}\n"
        "persistence_mode=aof\n",
        encoding="utf-8",
    )

    try:
        # 空目录启动，应创建空 AOF。
        process = launch(directory)
        wait_for_server(process, port)
        assert aof_path.read_bytes() == b""

        stop_server(process)
        process = None

        # 空 AOF 也应能正常重启。
        process = launch(directory)
        wait_for_server(process, port)
        assert aof_path.read_bytes() == b""

        prefixes = (b"", b"R", b"H", b"S")
        expected = {}

        for prefix in prefixes:
            value = b"aof " + prefix + b" value\r\nsecond line"
            expected[prefix] = value

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

            assert command(
                port, prefix + b"SET", b"empty", b""
            ) == b"OK"

            # 删除后重新新建，恢复时必须保持操作顺序。
            assert command(
                port, prefix + b"SET", b"reborn", b"old"
            ) == b"OK"
            assert command(port, prefix + b"DEL", b"reborn") == 1
            assert command(
                port, prefix + b"SET", b"reborn", b"new"
            ) == b"OK"

            # 重复 SET 应覆盖旧值，并追加一条日志。
            before_overwrite = aof_path.read_bytes()
            assert command(
                port, prefix + b"SET", b"name", b"overwrite"
            ) == b"OK"
            assert command(port, prefix + b"GET", b"name") == b"overwrite"
            expected[prefix] = b"overwrite"
            assert aof_path.read_bytes() == (
                before_overwrite
                + encode_command(prefix + b"SET", b"name", b"overwrite")
            )

            # 失败或未改变数据的操作，不应追加日志。
            before = aof_path.read_bytes()
            expect_error(port, prefix + b"MOD", b"missing", b"value")
            # 保留现有行为：Hash 删除缺失 key 返回错误。
            if prefix == b"H":
                expect_error(port, b"HDEL", b"missing")
            else:
                assert command(port, prefix + b"DEL", b"missing") == 0
            assert aof_path.read_bytes() == before

        # AOF 模式拒绝 SAVE，也不能因此生成快照或追加日志。
        before_save = aof_path.read_bytes()
        expect_error(port, b"SAVE")
        assert aof_path.read_bytes() == before_save

        for filename in ("snapshot.bin", "snapshot.db", "appendonly.aof"):
            assert not (directory / filename).exists()

        stop_server(process)
        process = None
        original = aof_path.read_bytes()
        assert original

        # 放置无效快照，证明 AOF 模式不会读取它们。
        (directory / "snapshot.bin").write_bytes(b"ignored snapshot")
        (directory / "snapshot.db").write_bytes(b"ignored legacy snapshot")

        # 连续重启两次：恢复正确，且回放不能再次追加 AOF。
        for _ in range(2):
            process = launch(directory)
            wait_for_server(process, port)

            for prefix in prefixes:
                assert command(
                    port, prefix + b"GET", b"name"
                ) == expected[prefix]
                assert command(
                    port, prefix + b"GET", b"deleted"
                ) is None
                assert command(
                    port, prefix + b"GET", b"empty"
                ) == b""
                assert command(
                    port, prefix + b"GET", b"reborn"
                ) == b"new"

            assert aof_path.read_bytes() == original

            stop_server(process)
            process = None
            assert aof_path.read_bytes() == original

        print("[PASS] AOF independent restart and replay without append")

        # 截断、格式错误、合法帧但业务回放失败，均应拒绝启动。
        bad_cases = {
            "truncated": original[:-1],
            "invalid frame": original + b"invalid frame\r\n",
            "MOD missing key": original + encode_command(
                b"HMOD", b"missing", b"value"
            ),
        }

        for name, content in bad_cases.items():
            aof_path.write_bytes(content)
            log_path = directory / "server.log"
            log_offset = log_path.stat().st_size

            process = launch(directory)
            try:
                return_code = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                raise AssertionError(f"{name}: 没有及时报错退出")

            assert return_code == 1, (
                f"{name}: 非预期退出码 {return_code}"
            )
            assert b"failed to replay standalone AOF" in (
                log_path.read_bytes()[log_offset:]
            )
            assert aof_path.read_bytes() == content

            process = None

        print("[PASS] AOF rejects invalid logs without modifying them")
        result = 0

    except Exception as exc:
        print(f"[FAIL] AOF restart: {exc!r}", file=sys.stderr)

    finally:
        stop_server(process)

    if result == 0:
        shutil.rmtree(directory)
    else:
        print(f"测试文件及日志保留在：{directory}", file=sys.stderr)

    return result


if __name__ == "__main__":
    sys.exit(main())