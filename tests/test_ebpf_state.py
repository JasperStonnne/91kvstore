"""Real uprobe/maps only: no new TCP path, config, or Replica handoff."""
import argparse
import errno
import json
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time

import test_rdma_replication as rdma


class Probe:
    def __init__(self, repo, pid, log_path):
        self.log = log_path.open("wb")
        self.process = subprocess.Popen(
            ["sudo", "-n", str(repo / "bin/ebpf-state-probe"), str(pid),
             str(repo / "build/ebpf_capture.bpf.o")],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
            text=True, bufsize=1,
        )
        try:
            assert self.line() == "READY", log_path.read_text(errors="replace")
        except BaseException:
            self.close(check=False)
            raise

    def line(self):
        assert select.select([self.process.stdout], [], [], 10)[0], "probe response timeout"
        line = self.process.stdout.readline()
        assert line, "probe exited; inspect probe log"
        return line.strip()

    def state(self, primary_id="0" * 40, sync_id="0" * 40, offset=0, cached=0):
        self.process.stdin.write(f"{primary_id} {sync_id} {offset} {cached}\n")
        self.process.stdin.flush()
        return json.loads(self.line())

    def close(self, check=True):
        # EOF runs kvs_ebpf_stop(): both links and all maps are owned by this helper.
        if self.process.stdin and not self.process.stdin.closed:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)
            raise
        finally:
            self.process.stdout.close()
            self.log.close()
        if check:
            assert self.process.returncode == 0, "state probe failed"


def request_boundary(port):
    control = socket.create_connection((rdma.TEST_HOST, port), timeout=10)
    stream = control.makefile("rb")
    try:
        control.sendall(b"PING\r\n")
        assert stream.readline() == b"PONG\r\n"
        control.sendall(b"PSYNC ? -1\r\n")
        parts = stream.readline().decode().split()
        assert len(parts) == 7 and parts[0] == "FULLRESYNC_RDMA", parts
        return control, stream, parts[1], parts[4], int(parts[5])
    except BaseException:
        stream.close()
        control.close()
        raise


def run(repo, root):
    fixture = rdma.Fixture(root, repo / "bin/kvstore")
    probe = writer = control = stream = None
    samples = []
    try:
        primary = fixture.start("primary")
        writer = rdma.Client(fixture.primary_port)
        seed = ("HSET", "color", "blue")
        assert writer.command(*seed) == b"OK"
        expected = len(rdma.frame(*seed))
        # Attach after a nonzero existing stream, then make no writes before PSYNC.
        probe = Probe(repo, primary.pid, root / "probe.log")
        assert probe.state()["read"] == -errno.EAGAIN
        control, stream, primary_id, sync_id, boundary = request_boundary(fixture.primary_port)
        assert boundary == expected > 0

        def sample(cached, old_sync=None):
            value = probe.state(primary_id, old_sync or sync_id, boundary, cached)
            samples.append(value)
            (root / "samples.json").write_text(json.dumps(samples, indent=2) + "\n")
            return value

        state = sample(boundary)
        assert state["read"] == state["match"] == state["progress"] == 0, state
        assert state["S"] == state["end"] == boundary and state["events"] == 0, state
        print("[PASS] nonzero S without any post-attach write; unknown boundary rejected", flush=True)

        writer.sock.sendall(rdma.frame("HMOD", "missing", "ignored"))
        assert writer.file.readline() == b"-ERR key not found\r\n"
        state = sample(boundary)
        assert state["end"] == boundary and state["events"] == 0, state
        command = ("HMOD", "color", "green")
        assert writer.command(*command) == b"OK"
        expected += len(rdma.frame(*command))
        state = sample(boundary)
        assert state["end"] == expected and state["events"] == 1, state
        assert state["progress"] == -errno.EAGAIN, state
        assert sample(expected)["progress"] == 0
        print("[PASS] failed write unchanged; successful write advances exact bytes; lag rejected", flush=True)

        # Successful command too large for an event; no later write to expose a gap.
        command = ("HSET", "large", "x" * 5000)
        assert writer.command(*command) == b"OK"
        expected += len(rdma.frame(*command))
        time.sleep(0.05)
        state = sample(expected)
        assert state["end"] == expected and state["events"] == 0, state
        assert state["oversize"] == 1 and state["match"] == state["progress"] == -errno.ENODATA, state
        print("[PASS] lost last oversize event: watermark advances and loss rejects equality", flush=True)

        # A newly loaded object must not inherit the already observed boundary.
        late = Probe(repo, primary.pid, root / "late-probe.log")
        try:
            assert late.state(primary_id, sync_id, boundary, expected)["read"] == -errno.EAGAIN
        finally:
            late.close()
        print("[PASS] late attach/restarted maps cannot claim the previous boundary", flush=True)

        stream.close()
        control.close()
        stream = control = None
        fixture.wait(lambda: "Replica control connection released" in
                     (root / "primary/server.log").read_text(), "control cleanup")
        old_sync = sync_id
        control, stream, primary_id, sync_id, boundary = request_boundary(fixture.primary_port)
        assert boundary == expected and sync_id != old_sync
        assert sample(boundary)["match"] == 0  # Snapshot covers the earlier oversize loss.
        assert sample(boundary, old_sync)["match"] == -errno.ESTALE
        print("[PASS] new full-sync identity/baseline accepted; previous identity rejected", flush=True)

        # Helper consumes no events until the next request: deterministically fill ring.
        command = ("HMOD", "color", "v")
        for _ in range(5000):
            assert writer.command(*command) == b"OK"
            expected += len(rdma.frame(*command))
        state = sample(boundary)
        assert state["end"] == expected and state["ring_full"] > 0, state
        assert state["match"] == state["progress"] == -errno.ENODATA, state
        print("[PASS] real ring full: last dropped writes still advance watermark, no later write needed", flush=True)
    finally:
        for resource in (stream, control, writer):
            if resource is not None:
                resource.close()
        try:
            if probe is not None:
                probe.close()
        finally:
            fixture.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path)
    parser.add_argument("--output", type=Path, help="new evidence directory, retained after test")
    args = parser.parse_args()
    repo = args.repo.resolve()
    rdma.TEST_HOST = rdma.discover_rdma_host()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
        run(repo, args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="91kv-ebpf-state-") as directory:
            run(repo, Path(directory))


if __name__ == "__main__":
    main()
