"""异步 SAVE 测试共用：区分请求接受与后台落盘完成。"""
import socket
import time


def snapshot_command(port, *fields):
    request = b"*" + str(len(fields)).encode() + b"\r\n"
    for field in fields:
        request += b"$" + str(len(field)).encode() + b"\r\n" + field + b"\r\n"
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.sendall(request)
        with sock.makefile("rb") as reply:
            line = reply.readline()
    assert line.startswith(b"+") and line.endswith(b"\r\n"), line
    return line[1:-2]


def save_info(port):
    return dict(item.split("=", 1) for item in snapshot_command(port, b"SAVEINFO").decode().split())


def wait_save(port, completed=0, failed=0, expect_failure=False, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = save_info(port)
        if state["in_progress"] == "0" and state["pending"] == "0":
            if expect_failure:
                assert int(state["failed"]) > failed, state
            else:
                assert int(state["completed"]) > completed and int(state["failed"]) == failed, state
            return state
        time.sleep(0.01)
    raise AssertionError(f"background save timeout: {state}")


def save_snapshot(port):
    before = save_info(port)
    assert snapshot_command(port, b"SAVE") == b"OK"
    wait_save(port, int(before["completed"]), int(before["failed"]))
    return b"OK"
