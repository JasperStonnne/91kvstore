"""Exercise checked AOF through real startup, dependent commands and restart."""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from test_aof_restart import encode_aof, encode_command, expect_error
from test_snapshot_restart import command, launch
from test_standalone import choose_port, stop_server, wait_for_server


def write_config(directory, port, checksum):
    contents = ("role=standalone\nbind_ip=127.0.0.1\n"
                f"service_port={port}\nallocator=pool\npersistence_mode=aof\n")
    if checksum is not None:
        contents += f"aof_checksum={checksum}\n"
    (directory / "server.conf").write_text(contents)


def expect_startup_failure(directory, content, diagnostic):
    path = directory / "commands.aof"
    path.write_bytes(content)
    log = directory / "server.log"
    position = log.stat().st_size
    process = launch(directory)
    try:
        assert process.wait(timeout=5) == 1
        output = log.read_bytes()[position:]
        assert diagnostic in output, output
        assert b"listen port :" not in output, output
        assert path.read_bytes() == content, "failed recovery changed the source file"
    finally:
        stop_server(process)


def run(directory):
    port = choose_port()
    path = directory / "commands.aof"
    write_config(directory, port, "file")
    commands = bytearray()
    process = None
    try:
        process = launch(directory)
        wait_for_server(process, port)
        assert path.read_bytes() == encode_aof(b"", "file")
        stop_server(process)
        process = launch(directory)
        wait_for_server(process, port)
        assert path.read_bytes() == encode_aof(b"", "file")
        for prefix in (b"", b"R", b"H", b"S"):
            for fields, expected in (
                    ((prefix + b"SET", b"key", b"first"), b"OK"),
                    ((prefix + b"MOD", b"key", b"modified\r\nvalue"), b"OK"),
                    ((prefix + b"SET", b"removed", b"temporary"), b"OK"),
                    ((prefix + b"DEL", b"removed"), 1)):
                assert command(port, *fields) == expected
                commands.extend(encode_command(*fields))
                assert path.read_bytes() == encode_aof(commands, "file")
        before_failed_command = path.read_bytes()
        expect_error(port, b"HMOD", b"missing", b"value")
        assert path.read_bytes() == before_failed_command
        for _ in range(2):
            stop_server(process)
            process = launch(directory)
            wait_for_server(process, port)
            assert path.read_bytes() == encode_aof(commands, "file")
            for prefix in (b"", b"R", b"H", b"S"):
                assert command(port, prefix + b"GET", b"key") == b"modified\r\nvalue"
                assert command(port, prefix + b"GET", b"removed") is None
            fields = (b"HSET", b"after_restart", b"kept")
            assert command(port, *fields) == b"OK"
            commands.extend(encode_command(*fields))
            assert path.read_bytes() == encode_aof(commands, "file")
        stop_server(process)
        process = None
        original = path.read_bytes()
        # 改动仍为合法 RESP 的旧命令内容，检查发生在回放前。
        damaged = bytearray(original)
        damaged[16 + commands.index(b"first")] ^= 1
        expect_startup_failure(directory, damaged, b"whole-file CRC32 mismatch")
        for position in (8, 12, len(original) - 12, len(original) - 1):
            damaged = bytearray(original)
            damaged[position] ^= 1
            expect_startup_failure(directory, damaged, b"aof:")
        for content in (original[:-1], original + b"x", b""):
            expect_startup_failure(directory, content, b"aof:")

        # CRC 正确的非法 RESP 和无法执行的命令也必须拒绝整次恢复。
        expect_startup_failure(directory, encode_aof(commands + b"broken", "file"),
                               b"failed to replay standalone AOF")
        bad = encode_command(b"HMOD", b"missing", b"value")
        expect_startup_failure(directory, encode_aof(commands + bad, "file"),
                               b"failed to replay standalone AOF")
        expect_startup_failure(directory, commands, b"aof_checksum=file requires")
        write_config(directory, port, "none")
        expect_startup_failure(directory, original, b"aof_checksum=none requires")

        # 省略配置仍能恢复旧纯 RESP 日志，并从原来的尾部继续追加。
        write_config(directory, port, None)
        path.write_bytes(commands)
        process = launch(directory)
        wait_for_server(process, port)
        assert command(port, b"HGET", b"after_restart") == b"kept"
        fields = (b"HMOD", b"after_restart", b"raw")
        assert command(port, *fields) == b"OK"
        commands.extend(encode_command(*fields))
        assert path.read_bytes() == commands
        assert not (directory / "snapshot.bin").exists()
        stop_server(process)
        process = None
        print("[PASS] AOF file: empty/restart/append, four-engine dependencies and exact CRC bytes")
        print("[PASS] AOF corruption and format mismatch rejected unchanged; legacy none compatible")
    finally:
        stop_server(process)


def main():
    root = Path(tempfile.mkdtemp(prefix="91kv-aof-checksum-"))
    try:
        run(root)
    except Exception:
        print(f"[FAIL] AOF checksum evidence retained: {root}", file=sys.stderr)
        raise
    shutil.rmtree(root)


if __name__ == "__main__":
    main()
