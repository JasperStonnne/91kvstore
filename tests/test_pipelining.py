#!/usr/bin/env python3
import socket
import sys
import uuid


def command(*fields):
    """把字段编码成一条长度前缀命令。"""
    parts = [b"*" + str(len(fields)).encode() + b"\r\n"]
    for field in fields:
        parts.extend((
            b"$" + str(len(field)).encode() + b"\r\n",
            field,
            b"\r\n",
        ))
    return b"".join(parts)


def receive_expected(sock, expected):
    """读取预期长度的回复，并报告完成、超时或连接关闭。"""
    received = bytearray()

    while len(received) < len(expected):
        try:
            chunk = sock.recv(len(expected) - len(received))
        except socket.timeout:
            return bytes(received), "timeout"

        if not chunk:
            return bytes(received), "closed"

        received.extend(chunk)

    return bytes(received), "complete"


def test_pipelined(port):
    """同一连接一次发送两条命令，两条都应得到回复。"""
    key = ("pipeline_" + uuid.uuid4().hex).encode()
    request = command(b"HSET", key, b"v") + command(b"HGET", key)
    expected = b"OK\r\n$1\r\nv\r\n"

    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
        sock.settimeout(2)
        sock.sendall(request)
        received, state = receive_expected(sock, expected)

    passed = state == "complete" and received == expected
    return passed, f"received={received!r}, state={state}, expected={expected!r}"


def test_subsequent_recv(port):
    """先收到第一条回复，再在同一连接发送下一条命令。"""
    key = ("sequential_" + uuid.uuid4().hex).encode()
    expected_first = b"OK\r\n"
    expected_second = b"$1\r\nv\r\n"

    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
        sock.settimeout(2)

        sock.sendall(command(b"HSET", key, b"v"))
        first, first_state = receive_expected(sock, expected_first)
        if first_state != "complete" or first != expected_first:
            return False, f"first={first!r}, state={first_state}"

        sock.sendall(command(b"HGET", key))
        second, second_state = receive_expected(sock, expected_second)

    passed = second_state == "complete" and second == expected_second
    return passed, (
        f"first={first!r}, second={second!r}, "
        f"second_state={second_state}, expected={expected_second!r}"
    )


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <port>", file=sys.stderr)
        return 2

    port = int(sys.argv[1])
    failures = 0

    for name, test in (
        ("pipelined commands", test_pipelined),
        ("subsequent recv", test_subsequent_recv),
    ):
        try:
            passed, detail = test(port)
        except OSError as exc:
            passed, detail = False, f"socket error: {exc}"

        print(f"[{'PASS' if passed else 'FAIL'}] {name}: {detail}")
        if not passed:
            failures += 1

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())