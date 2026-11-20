"""Real RDMA socket-cache overflow; capture TCP faults live in test_capture_server.py.

Actual Replica collector exit/tail loss/control loss and full-sync retry are exercised
by test_capture_replication.py, sharing the same DATA/APPLIED production path.
"""
import argparse
from pathlib import Path
import tempfile
import test_capture_replication as capture

def socket_overflow(repo, root):
    directory = root / "socket-overflow"
    directory.mkdir()
    fixture = capture.Fixture(directory, repo / "bin/kvstore", "socket")
    writer = reader = gate = None
    try:
        fixture.start("primary")
        writer = capture.rdma.Client(fixture.primary_port)
        gate = capture.Gate(fixture.primary_port)
        fixture.start("replica", gate.port)
        assert gate.header_ready.wait(10)
        gate.check()
        value = "x" * 4000
        for index in range(2100):
            assert writer.command("HSET", f"overflow_{index}", value) == b"OK"
        gate.release.set()
        primary_log = directory / "primary/server.log"
        fixture.wait(lambda: "catch-up cache unavailable" in primary_log.read_text() and
                     "Replica control connection released" in primary_log.read_text(),
                     "overflow session not released", timeout=15)
        gate.thread.join(timeout=5)
        assert not gate.thread.is_alive(), "Primary did not close relay upstream"
        gate.check()
        assert not gate.wire, "overflow session delivered increments or ONLINE"
        # 原有暂停代理不主动向下游传 EOF；确认真实上游 EOF 后关闭下游。
        gate.close()
        gate = None
        log = directory / "replica/server.log"
        fixture.wait(lambda: "Replica control connection closed" in log.read_text(),
                     "Replica did not observe relay EOF", timeout=10)
        assert "replication: enter ONLINE" not in log.read_text()
        fixture.stop(fixture.processes[-1][0])
        fixture.start("replica", fixture.primary_port)
        fixture.online()
        reader = capture.rdma.Client(fixture.replica_port)
        for index in range(2100):
            assert reader.command("HGET", f"overflow_{index}") == value.encode()
        capture.rdma.assert_rdma_snapshot(fixture)
        print("[PASS] socket cache overflow: no ONLINE, fresh full sync verifies all 2100 writes", flush=True)
    finally:
        if reader:
            reader.close()
        if writer:
            writer.close()
        try:
            if gate:
                gate.close()
        finally:
            fixture.close()



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path)
    args = parser.parse_args()
    capture.rdma.TEST_HOST = capture.rdma.discover_rdma_host()
    root = Path(tempfile.mkdtemp(prefix="91kv-capture-faults-"))
    print(f"Evidence: {root}", flush=True)
    socket_overflow(args.repo.resolve(), root)


if __name__ == "__main__":
    main()
