#!/usr/bin/env python3
"""Real kvstore comm resolution/error paths; missing object prevents privileged BPF load."""
from pathlib import Path
import os
import subprocess
import tempfile

from test_standalone import choose_port, wait_for_server, stop_server

REPO = Path(__file__).resolve().parent.parent


def main():
    owned = []
    try:
        with tempfile.TemporaryDirectory(prefix="91kv-process-target-") as directory:
            root = Path(directory)
            def lookup(target):
                result = subprocess.run([str(REPO / "bin/ebpf_capture"), target,
                    str(root / "intentionally-missing.bpf.o")], capture_output=True, text=True, timeout=10)
                assert result.returncode != 0, result
                return result.stderr
            def start(index):
                config = root / f"server-{index}.conf"
                port = choose_port()
                config.write_text(f"role=standalone\nbind_ip=127.0.0.1\nservice_port={port}\n"
                                  "log_level=error\npersistence_mode=none\n")
                process = subprocess.Popen([str(REPO / "bin/kvstore"), str(config)], cwd=root,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                owned.append(process)
                wait_for_server(process, port)
                return process
            # Refuse to confuse test-owned fixtures with an unrelated running instance.
            external = []
            for path in Path("/proc").glob("[0-9]*/comm"):
                try:
                    if path.read_text().rstrip("\n") == "kvstore": external.append(path.parent.name)
                except OSError:
                    pass
            assert not external, f"unrelated kvstore instances: {external}"
            missing = f"s41no{os.getpid()}"
            text = lookup(missing)
            assert "not found" in text and "attach replication probes" not in text, text
            print("[INFO] missing target rejected: " + text.strip(), flush=True)
            first = start(1)
            text = lookup("kvstore")
            assert f"resolved process 'kvstore' to PID {first.pid}" in text, text
            assert "attach replication probes" in text, text
            print("[INFO] unique target resolved (BPF object intentionally absent): " + text.splitlines()[0], flush=True)
            text = lookup(str(first.pid))
            assert "resolved process" not in text and "attach replication probes" in text, text
            second = start(2)
            text = lookup("kvstore")
            assert "ambiguous process" in text and "attach replication probes" not in text, text
            assert str(first.pid) in text and str(second.pid) in text, text
            print("[INFO] multiple instances rejected before attach: " + text.strip(), flush=True)
            for target in ["0", "123abc", "abcdefghijklmnop", ""]:
                assert "attach replication probes" not in lookup(target)
            for process in owned:stop_server(process)
            assert all(process.poll() == 0 for process in owned)
            assert "not found" in lookup("kvstore")
        print("[PASS] actual kvstore CLI: unique/numeric, two-instance conflict before attach, missing name, invalid input and owned process cleanup")
    finally:
        for process in owned:stop_server(process)


if __name__ == "__main__":
    main()
