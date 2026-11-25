"""Real BPF -> independent TCP port -> validating test client, not the Replica handoff."""
import argparse
import json
from pathlib import Path
import select
import socket
import subprocess
import tempfile

import test_rdma_replication as rdma
from test_ebpf_state import request_boundary
from test_capture_server import Client, frame


# The sudo owner only signals the exact child it created; EOF requests graceful cleanup.
OWNER = """import select,subprocess,sys
p=subprocess.Popen(sys.argv[1:])
try:
 while p.poll() is None:
  if select.select([sys.stdin],[],[],.1)[0]:
   sys.stdin.readline()
   break
finally:
 if p.poll() is None:
  p.terminate()
  try: p.wait(timeout=8)
  except subprocess.TimeoutExpired: p.kill(); p.wait(timeout=3)
raise SystemExit(p.returncode)
"""


class Collector:
    def __init__(self, repo, pid, port, log_path, fixture, binary_name="kvstore-ebpf-capture"):
        self.log = log_path.open("wb")
        self.process = subprocess.Popen(
            ["sudo", "-n", "python3", "-u", "-c", OWNER,
             str(repo / "bin" / binary_name), str(pid),
             str(repo / "build/ebpf_capture.bpf.o"), "--listen", f"{rdma.TEST_HOST}:{port}"],
            stdin=subprocess.PIPE, stdout=self.log, stderr=self.log)
        try:
            fixture.wait(lambda: "capture TCP listening" in log_path.read_text(errors="replace") or
                         self.process.poll() is not None, "capture TCP did not listen", timeout=10)
            assert self.process.poll() is None, log_path.read_text(errors="replace")
        except BaseException:
            self.close(check=False)
            raise

    def close(self, check=True):
        if self.process.stdin and not self.process.stdin.closed:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=12)
        except subprocess.TimeoutExpired:
            raise AssertionError("sudo owner did not clean its collector")
        finally:
            self.log.close()
        if check:
            assert self.process.returncode == 0, "collector failed; inspect its log"


def run(repo, root):
    fixture = rdma.Fixture(root, repo / "bin/kvstore")
    writer = collector = client = control = stream = None
    results = []
    completed = False
    try:
        primary = fixture.start("primary")
        writer = rdma.Client(fixture.primary_port)
        assert writer.command("HSET", "color", "blue") == b"OK"
        expected = len(frame("HSET", "color", "blue"))
        port = rdma.free_port()
        while port in (fixture.primary_port, fixture.rdma_port, fixture.replica_port):
            port = rdma.free_port()
        collector = Collector(repo, "kvstore", port, root / "capture-1.log", fixture,
                              binary_name="ebpf_capture")
        assert f"resolved process 'kvstore' to PID {primary.pid}" in (root / "capture-1.log").read_text()
        print(f"[PASS] process-name kvstore resolved and real uprobe attached to PID {primary.pid}", flush=True)
        control, stream, primary_id, sync_id, start = request_boundary(fixture.primary_port)
        assert start == expected
        commands = [("HMOD", "color", "green"), ("HSET", "extra", "value")]
        for command in commands:
            assert writer.command(*command) == b"OK"
            expected += len(frame(*command))
        client = Client(port, start, host=rdma.TEST_HOST)
        client.values[b"color"] = b"blue"  # Snapshot content is supplied by the test, not loaded here.
        client.begin(primary_id, sync_id, start, fragmented=True)
        wire = bytearray()
        while client.received < expected:
            kind, payload = client.read()
            assert kind == "DATA"
            wire.extend(payload)
            client.ack()
        assert wire == b"".join(frame(*command) for command in commands)
        assert client.read() == ("ONLINE", expected)
        assert client.count == 2 and client.applied == expected
        results.append("exact-catchup-online")
        print(f"[PASS] real BPF direct TCP: S={start}, exact catch-up, ONLINE {expected}", flush=True)

        writer.sock.sendall(frame("HMOD", "missing", "ignored"))
        assert writer.file.readline() == b"-ERR key not found\r\n"
        client.quiet()
        command = ("HMOD", "color", "online")
        assert writer.command(*command) == b"OK"
        expected += len(frame(*command))
        assert client.read() == ("DATA", frame(*command))
        client.ack()
        assert client.applied == expected and client.count == 3 and client.values[b"color"] == b"online"
        assert not select.select([control], [], [], 0.05)[0], "Primary forwarded increment bytes"
        results.append("live-and-failed-write")
        print("[PASS] failed write emits nothing; live bytes direct from collector, no Primary forwarding", flush=True)

        for _ in range(21):
            for _ in range(100):
                command = ("HSET", "blob", "x" * 4000)
                assert writer.command(*command) == b"OK"
                expected += len(frame(*command))
            while client.received < expected:
                assert client.read()[0] == "DATA"
                client.ack()
            assert client.applied == expected
        assert client.count == 2103 and expected - start > 8 * 1024 * 1024
        assert client.values[b"blob"] == b"x" * 4000
        client.quiet()
        results.append("wrap-exact-once")
        print("[PASS] real BPF >8MiB wrap: contiguous offsets, 2103 commands each applied once", flush=True)

        collector.close()
        collector = None
        try:
            client.read()
            raise AssertionError("collector exit did not disconnect test client")
        except (EOFError, ConnectionResetError):
            pass
        client.close()
        client = None
        collector = Collector(repo, primary.pid, port, root / "capture-2.log", fixture)
        client = Client(port, start, host=rdma.TEST_HOST)
        client.begin(primary_id, sync_id, start)
        assert client.read() == ("ERROR", "NO_BOUNDARY")
        client.close()
        client = None
        results.append("restart-rejects-old-boundary")
        print("[PASS] restarted collector lost cache/maps: previous full-sync boundary rejected", flush=True)

        stream.close()
        control.close()
        stream = control = None
        fixture.wait(lambda: "Replica control connection released" in
                     (root / "primary/server.log").read_text(), "Primary did not release control")
        control, stream, primary_id, sync_id, start = request_boundary(fixture.primary_port)
        assert start == expected
        client = Client(port, start, host=rdma.TEST_HOST)
        client.begin(primary_id, sync_id, start)
        assert client.read() == ("ONLINE", start)
        results.append("fresh-boundary-without-new-write")
        print("[PASS] fresh full-sync boundary after restart: no new write needed for ONLINE S", flush=True)
        assert writer.command("HSET", "oversize", "x" * 5000) == b"OK"
        assert client.read() == ("ERROR", "CAPTURE_LOSS")
        results.append("last-loss-without-next-write")
        print("[PASS] real BPF last oversize event lost: direct TCP reports CAPTURE_LOSS without next write", flush=True)
        completed = True
    finally:
        for resource in (client, stream, control, writer):
            if resource is not None:
                resource.close()
        cleanup_ok = collector_ok = False
        try:
            if collector is not None:
                collector.close()
            collector_ok = True
        finally:
            try:
                fixture.close()
                cleanup_ok = True
            finally:
                (root / "result.json").write_text(json.dumps(
                    {"passed": completed and cleanup_ok and collector_ok and len(results) == 6,
                     "cases": results}, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    subprocess.run(["sudo", "-n", "true"], check=True)
    rdma.TEST_HOST = rdma.discover_rdma_host()
    root = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="91kv-capture-direct-bpf-"))
    if args.output:
        root.mkdir(parents=True, exist_ok=False)
    print(f"Evidence: {root}", flush=True)
    run(args.repo.resolve(), root)


if __name__ == "__main__":
    main()
