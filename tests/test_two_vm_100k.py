#!/usr/bin/env python3
"""Opt-in two-machine acceptance client; never starts or stops servers.

Run write batch 1 before starting Replica, then write batch 2 after its
startup, and verify all records through Replica. Evidence about overlap and
AOF catch-up must additionally come from the server logs.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import socket
import sys
import time


CASES = (("Array", b"", 500), ("RBTree", b"R", 16500),
         ("Hash", b"H", 16500), ("Skiplist", b"S", 16500))


def record(run_id, batch, prefix, index):
    key = f"{run_id}_b{batch}_{prefix.decode() or 'A'}_{index:06d}".encode()
    value = b"value " + key + b" " + b"x" * (index % 97 + 1) + b"\r\nsecond line"
    return key, value


class ProtocolError(Exception):
    pass


class Client:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(30)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.reader = self.sock.makefile("rb")

    def close(self):
        self.reader.close()
        self.sock.close()

    def command(self, *fields):
        self.sock.sendall(b"*%d\r\n" % len(fields) + b"".join(
            b"$%d\r\n" % len(x) + x + b"\r\n" for x in fields))
        line = self.reader.readline(1024)
        if not line.endswith(b"\r\n"):
            raise ProtocolError(f"incomplete reply: {line!r}")
        if line[:1] == b"+":
            return "simple", line[1:-2]
        if line[:1] == b"$":
            text = line[1:-2]
            if text == b"-1":
                return "bulk", None
            if not text or not text.isdigit():
                raise ProtocolError(f"invalid bulk length: {line!r}")
            length = int(text)
            if length > 1024 * 1024:
                raise ProtocolError("unexpected bulk size above 1 MiB")
            data = self.reader.read(length + 2)
            if len(data) != length + 2 or data[-2:] != b"\r\n":
                raise ProtocolError("incomplete bulk body")
            return "bulk", data[:-2]
        raise ProtocolError(f"unexpected reply: {line!r}")


def run(args):
    batches = (1, 2) if args.batch == "all" else (int(args.batch),)
    report = Path(args.report)
    # Exclusive creation prevents silently replacing evidence from a prior run.
    with report.open("x", encoding="utf-8") as log:
        def event(kind, **fields):
            item = dict(event=kind, utc=datetime.now(timezone.utc).isoformat(), **fields)
            line = json.dumps(item, ensure_ascii=False)
            print(line, flush=True)
            log.write(line + "\n")
            log.flush()

        counts = {str(b): dict(accepted=0, matched=0, missing=0,
                              mismatched=0, protocol_error=0) for b in batches}
        started = time.monotonic()
        event("start", mode=args.mode, host=args.host, port=args.port,
              run_id=args.run_id, batches=list(batches),
              per_batch=sum(n for _, _, n in CASES),
              engines=[dict(name=n, per_batch=c) for n, _, c in CASES])
        client = None
        current_batch = None
        current_key = None
        try:
            client = Client(args.host, args.port)
            completed = 0
            for batch in batches:
                current_batch = str(batch)
                stats = counts[current_batch]
                for name, prefix, count in CASES:
                    for index in range(count):
                        key, expected = record(args.run_id, batch, prefix, index)
                        current_key = key.decode()
                        if args.mode == "write":
                            kind, value = client.command(prefix + b"SET", key, expected)
                            if (kind, value) != ("simple", b"OK"):
                                raise ProtocolError(f"SET requires +OK, got {(kind, value)!r}")
                            stats["accepted"] += 1
                        else:
                            kind, value = client.command(prefix + b"GET", key)
                            if kind != "bulk":
                                raise ProtocolError(f"GET requires bulk reply, got {kind}")
                            if value is None:
                                stats["missing"] += 1
                            elif value != expected:
                                stats["mismatched"] += 1
                            else:
                                stats["matched"] += 1
                        completed += 1
                        if completed % 10000 == 0:
                            event("progress", completed=completed, counts=counts)
                    event("engine_complete", batch=batch, engine=name, records=count)
            expected_count = sum(n for _, _, n in CASES)
            success = all(s["accepted" if args.mode == "write" else "matched"]
                          == expected_count for s in counts.values())
            event("complete", success=success, counts=counts,
                  seconds=round(time.monotonic() - started, 3))
            return 0 if success else 1
        except Exception as exc:
            if isinstance(exc, ProtocolError) and current_batch is not None:
                counts[current_batch]["protocol_error"] += 1
            event("failed", error=repr(exc), batch=current_batch, key=current_key,
                  counts=counts, seconds=round(time.monotonic() - started, 3))
            # On a broken connection the rest are unverified, never counted as missing.
            return 1
        finally:
            if client is not None:
                client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("write", "verify"))
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--batch", required=True, choices=("1", "2", "all"))
    parser.add_argument("--run-id", required=True,
                        help="same unique ASCII key prefix for all three invocations")
    parser.add_argument("--report", required=True, help="new JSONL file; must not exist")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be in 1..65535")
    if not args.run_id.isascii() or not args.run_id.replace("_", "").isalnum():
        parser.error("run-id must contain only ASCII letters, digits and underscores")
    if args.mode == "write" and args.batch == "all":
        parser.error("write one batch at a time, so Replica can start between batches")
    return run(args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        sys.exit(1)
