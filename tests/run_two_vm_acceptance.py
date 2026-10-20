#!/usr/bin/env python3
"""Opt-in Linux two-VM orchestration. Run on the Primary VM.

Requires sibling test_two_vm_100k.py. SSH authenticates once through a private
control socket; no password/key is collected by this program. All servers use
new directories. Existing servers are neither reused nor stopped.
"""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace

import test_two_vm_100k as workload


def utc():
    return datetime.now(timezone.utc).isoformat()


def require(value, message):
    if not value:
        raise RuntimeError(message)


# The same process-ownership checks run on both machines. argv and /proc start
# time must match the saved owner record before any signal can be sent.
HELPER = r'''
import hashlib, json, os, pathlib, signal, socket, subprocess, sys, tempfile, time
from datetime import datetime, timezone
action, arg = sys.argv[1], json.loads(sys.argv[2])
def identity(pid):
    try:
        raw = pathlib.Path(f'/proc/{pid}/stat').read_text()
        tail = raw[raw.rfind(')') + 2:].split()
        return tail[0], tail[19]
    except FileNotFoundError:
        return None, None
def metadata(repo):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(repo), *args], text=True)
    files = git('ls-files', 'src', 'include').splitlines()
    hashes = {p: hashlib.sha256((repo/p).read_bytes()).hexdigest()
              for p in files if p.endswith(('.c', '.h'))}
    return dict(head=git('rev-parse', 'HEAD').strip(),
                status=git('status', '--short'), diff=git('diff', 'HEAD'),
                source_hashes=hashes, submodules=git('submodule', 'status'),
                kernel=os.uname().release, architecture=os.uname().machine,
                binary_sha256=hashlib.sha256((repo/'bin/kvstore').read_bytes()).hexdigest())
def listeners(pid):
    inodes = set()
    for fd in pathlib.Path(f'/proc/{pid}/fd').iterdir():
        try: target = os.readlink(fd)
        except FileNotFoundError: continue
        if target.startswith('socket:['): inodes.add(target[8:-1])
    result = []
    for table in ('tcp', 'tcp6'):
        for line in pathlib.Path(f'/proc/{pid}/net/{table}').read_text().splitlines()[1:]:
            fields = line.split()
            if fields[9] in inodes and fields[3] == '0A':
                address, port = fields[1].split(':')
                host = socket.inet_ntoa(bytes.fromhex(address)[::-1]) if table == 'tcp' else address
                result.append(dict(host=host, port=int(port, 16)))
    return result
if action == 'init':
    repo = pathlib.Path(arg['repo']).resolve()
    binary = repo/'bin/kvstore'
    assert binary.is_file() and os.access(binary, os.X_OK), f'build first: {binary}'
    with socket.socket() as probe: probe.bind((arg['host'], arg['port']))
    parent = pathlib.Path(arg['parent']).resolve()
    assert parent.is_dir(), f'artifact parent does not exist: {parent}'
    root = pathlib.Path(tempfile.mkdtemp(prefix='91kv-auto-' + arg['role'] + '-', dir=parent))
    config = root/'server.conf'
    content = (f"role={arg['role']}\nbind_ip={arg['host']}\nservice_port={arg['port']}\n"
               'log_level=info\npersistence_mode=legacy_hybrid\n')
    if arg['role'] == 'replica':
        content += f"primary_host={arg['upstream_host']}\nprimary_port={arg['upstream_port']}\n"
    config.write_text(content)
    print(json.dumps(dict(root=str(root), config=str(config), binary=str(binary),
                          metadata=metadata(repo))))
elif action == 'start':
    root = pathlib.Path(arg['root'])
    owner = root/'owner.json'
    assert not owner.exists(), 'this directory already started a server'
    binary = str(pathlib.Path(arg['binary']).resolve())
    config = str(root/'server.conf')
    started = datetime.now(timezone.utc).isoformat()
    with (root/'server.log').open('xb') as log:
        process = subprocess.Popen(['stdbuf', '-oL', binary, config], cwd=root,
            stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
            start_new_session=True)
    state, tick = identity(process.pid)
    data = dict(pid=process.pid, start_tick=tick, binary=binary, config=config, utc=started)
    owner.write_text(json.dumps(data))
    print(json.dumps(data))
elif action == 'log':
    print(json.dumps((pathlib.Path(arg['root'])/'server.log').read_text(errors='replace')))
elif action in ('status', 'stop'):
    root = pathlib.Path(arg['root'])
    owner = json.loads((root/'owner.json').read_text())
    pid = owner['pid']
    state, tick = identity(pid)
    alive = state not in (None, 'Z') and tick == owner['start_tick']
    if alive:
        argv = pathlib.Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
        assert owner['binary'].encode() in argv and owner['config'].encode() in argv, 'PID ownership mismatch'
    if action == 'status':
        print(json.dumps(dict(alive=alive, pid=pid, listeners=listeners(pid) if alive else [])))
    else:
        forced = False
        if alive:
            os.kill(pid, signal.SIGTERM)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                state, tick = identity(pid)
                if state in (None, 'Z') or tick != owner['start_tick']: break
                time.sleep(.05)
            else:
                # Recheck argv and start time immediately before forced cleanup.
                argv = pathlib.Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
                assert identity(pid)[1] == owner['start_tick'] and owner['config'].encode() in argv
                os.kill(pid, signal.SIGKILL)
                forced = True
        state, tick = identity(pid)
        stopped = state in (None, 'Z') or tick != owner['start_tick']
        if forced:
            for _ in range(40):
                if stopped: break
                time.sleep(.05)
                state, tick = identity(pid)
                stopped = state in (None, 'Z') or tick != owner['start_tick']
        print(json.dumps(dict(pid=pid, stopped=stopped, forced=forced)))
'''


@contextmanager
def phase_timeout(seconds):
    def expired(signum, frame):
        raise TimeoutError(f'phase exceeded {seconds} seconds')
    previous = signal.signal(signal.SIGALRM, expired)
    signal.setitimer(signal.ITIMER_REAL, seconds)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)


class Remote:
    def __init__(self, target, port):
        self.target = target
        # A short private /tmp path avoids the Unix socket path length limit.
        self.control_dir = tempfile.TemporaryDirectory(prefix='91kv-ssh-')
        os.chmod(self.control_dir.name, 0o700)
        self.control = str(Path(self.control_dir.name)/'control')
        self.base = ['ssh', '-p', str(port), '-S', self.control,
                     '-o', 'ConnectTimeout=10', '-o', 'ServerAliveInterval=10',
                     '-o', 'ServerAliveCountMax=3']
        self.closed = False
        try:
            # Authentication inherits the user's terminal: at most one login.
            # Host-key validation remains enabled; no passwords enter our logs.
            subprocess.run(self.base + ['-M', '-N', '-f', '-o', 'ControlPersist=600', target],
                           check=True, timeout=90)
        except BaseException:
            self.close()
            raise

    def call(self, action, arg):
        command = shlex.join(['python3', '-c', HELPER, action, json.dumps(arg)])
        result = subprocess.run(self.base + ['-o', 'BatchMode=yes', self.target, command],
                                capture_output=True, text=True, timeout=30)
        require(result.returncode == 0, f'SSH {action} failed: {result.stderr[-3000:]}')
        return json.loads(result.stdout)

    def close(self):
        if self.closed:
            return
        self.closed = True
        if Path(self.control).exists():
            subprocess.run(self.base + ['-O', 'exit', self.target],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
        self.control_dir.cleanup()


def local_call(action, arg):
    result = subprocess.run([sys.executable, '-c', HELPER, action, json.dumps(arg)],
                            capture_output=True, text=True, timeout=30)
    require(result.returncode == 0, f'local {action} failed: {result.stderr[-3000:]}')
    return json.loads(result.stdout)


class Gate:
    """Test-only relay. Constrain upstream buffering and hold Snapshot bytes."""
    def __init__(self, host, upstream_port, event):
        self.host, self.upstream_port, self.event = host, upstream_port, event
        self.listener = socket.socket()
        self.listener.bind((host, 0))
        self.listener.listen(1)
        self.listener.settimeout(30)
        self.port = self.listener.getsockname()[1]
        self.ready = threading.Event()
        self.release = threading.Event()
        self.stopped = threading.Event()
        self.errors = []
        self.sockets = [self.listener]
        self.metadata = None
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def line(self, sock):
        result = bytearray()
        while not result.endswith(b'\r\n'):
            chunk = sock.recv(1)
            require(chunk, 'relay disconnected during control line')
            result.extend(chunk)
            require(len(result) <= 512, 'relay control line too long')
        return bytes(result)

    def run(self):
        try:
            downstream, _ = self.listener.accept()
            downstream.settimeout(30)
            self.sockets.append(downstream)
            upstream = socket.socket()
            self.sockets.append(upstream)
            upstream.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            upstream.settimeout(30)
            upstream.connect((self.host, self.upstream_port))
            require(self.line(downstream) == b'PING\r\n', 'unexpected Replica PING')
            upstream.sendall(b'PING\r\n')
            require(self.line(upstream) == b'PONG\r\n', 'unexpected Primary PONG')
            downstream.sendall(b'PONG\r\n')
            require(self.line(downstream) == b'PSYNC ? -1\r\n', 'unexpected PSYNC')
            upstream.sendall(b'PSYNC ? -1\r\n')
            header = self.line(upstream)
            parts = header.split()
            require(len(parts) == 4 and parts[0] == b'FULLRESYNC', 'invalid FULLRESYNC')
            self.metadata = dict(offset=int(parts[2]), size=int(parts[3]))
            downstream.sendall(header)
            self.event('snapshot_held', **self.metadata)
            self.ready.set()
            require(self.release.wait(120), 'Snapshot gate release timed out')
            upstream.settimeout(.5)
            while not self.stopped.is_set():
                try:
                    chunk = upstream.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                downstream.sendall(chunk)
        except Exception as exc:
            if not self.stopped.is_set():
                self.errors.append(repr(exc))
                self.ready.set()

    def check(self):
        require(not self.errors, f'relay failed: {self.errors}')

    def close(self):
        self.stopped.set()
        self.release.set()
        for sock in list(self.sockets):
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()
        self.thread.join(5)
        require(not self.thread.is_alive(), 'relay thread did not stop')


class Suite:
    def __init__(self, args, root, remote):
        self.args, self.root, self.remote = args, root, remote
        self.primary = self.replica = self.gate = None
        self.started = []
        self.events = (root/'events.jsonl').open('x', encoding='utf-8')
        self.lock = threading.Lock()

    def event(self, name, **fields):
        item = dict(event=name, utc=utc(), **fields)
        with self.lock:
            self.events.write(json.dumps(item, ensure_ascii=False) + '\n')
            self.events.flush()
            print(f'[{name}] ' + json.dumps(fields, ensure_ascii=False), flush=True)

    def init(self, role, upstream_port=None):
        a = self.args
        data = dict(repo=str(a.repo) if role == 'primary' else a.replica_repo,
                    role=role, host=a.primary_host if role == 'primary' else a.replica_host,
                    port=a.primary_port if role == 'primary' else a.replica_port,
                    parent=str(self.root) if role == 'primary' else a.replica_artifacts,
                    upstream_host=a.primary_host, upstream_port=upstream_port)
        call = local_call if role == 'primary' else self.remote.call
        result = call('init', data)
        (self.root/f'{role}-metadata.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
        self.event('directory', role=role, path=result['root'])
        return result

    def start(self, role):
        info = self.primary if role == 'primary' else self.replica
        call = local_call if role == 'primary' else self.remote.call
        # Register before start so cleanup can inspect owner.json if SSH is interrupted.
        self.started.append((role, call, info))
        result = call('start', info)
        self.event('server_started', role=role, owner=result)
        expected = dict(host=self.args.primary_host if role == 'primary' else self.args.replica_host,
                        port=self.args.primary_port if role == 'primary' else self.args.replica_port)
        def ready():
            status = call('status', info)
            require(status['alive'], f'{role} exited during startup')
            if status['listeners']:
                require(status['listeners'] == [expected], f'unexpected {role} listeners: {status}')
                return True
            return False
        self.wait(ready, f'{role} listener', 30)
        self.event('listener_verified', role=role, **expected)

    def log(self, role):
        return (local_call if role == 'primary' else self.remote.call)(
            'log', self.primary if role == 'primary' else self.replica)

    def wait(self, predicate, description, timeout=None):
        deadline = time.monotonic() + (timeout or self.args.phase_timeout)
        while time.monotonic() < deadline:
            if self.gate:
                self.gate.check()
            if predicate():
                self.event('condition_observed', description=description)
                return
            time.sleep(.1)
        raise TimeoutError(f'timed out waiting for {description}')

    def online(self):
        self.wait(lambda: 'replication: enter ONLINE' in self.log('replica'), 'Replica ONLINE')

    def client(self, role):
        return workload.Client(self.args.primary_host if role == 'primary' else self.args.replica_host,
                               self.args.primary_port if role == 'primary' else self.args.replica_port)

    def write_checked(self, client, key, value):
        require(client.command(b'HSET', key, value) == ('simple', b'OK'), f'HSET failed: {key!r}')

    def prepare(self, controlled):
        self.primary = self.init('primary')
        if controlled:
            self.gate = Gate(self.args.primary_host, self.args.primary_port, self.event)
            self.event('test_relay_listener', host=self.args.primary_host, port=self.gate.port)
        self.replica = self.init('replica', self.gate.port if controlled else self.args.primary_port)
        left, right = self.primary['metadata'], self.replica['metadata']
        require(left['head'] == right['head'], 'VM Git HEAD differs')
        require(left['source_hashes'] == right['source_hashes'], 'tracked src/include contents differ between VMs')
        self.event('source_versions_match', head=left['head'], note='working-tree metadata and patches saved')
        self.start('primary')

    def workload(self, mode, batch, role):
        report = self.root/f'{role}-{mode}-{batch}.jsonl'
        args = SimpleNamespace(mode=mode, batch=batch, run_id=self.args.run_id,
            host=self.args.primary_host if role == 'primary' else self.args.replica_host,
            port=self.args.primary_port if role == 'primary' else self.args.replica_port,
            report=str(report))
        self.event('workload_start', mode=mode, batch=batch, role=role)
        with phase_timeout(self.args.phase_timeout):
            result = workload.run(args)
        require(result == 0, f'{role} {mode} batch {batch} failed; see {report}')
        summary = json.loads(report.read_text().splitlines()[-1])
        self.event('workload_complete', role=role, mode=mode, batch=batch, summary=summary)
        return summary

    def marker(self, key, expected):
        def matches():
            client = None
            try:
                client = self.client('replica')
                return client.command(b'SGET', key) == ('bulk', expected)
            except OSError:
                return False
            finally:
                if client:
                    client.close()
        self.wait(matches, 'last batch-2 record readable from Replica')

    def ranges(self):
        return [(int(a), int(b)) for a, b in re.findall(
            r'start AOF catch-up range=\[(\d+),(\d+)\)', self.log('primary'))]

    def direct(self):
        self.prepare(False)
        self.workload('write', '1', 'primary')
        self.start('replica')
        self.workload('write', '2', 'primary')
        self.online()
        _, prefix, count = workload.CASES[-1]
        key, value = workload.record(self.args.run_id, 2, prefix, count - 1)
        self.marker(key, value)
        replica = self.workload('verify', 'all', 'replica')
        primary = self.workload('verify', 'all', 'primary')
        ranges = self.ranges()
        return dict(scenario='direct_two_vm_50000_plus_50000', success=True,
                    replica=replica['counts'], primary=primary['counts'],
                    catchup_ranges=ranges, nonempty_catchup_proven=any(b > a for a, b in ranges))

    def verify_hash_records(self, records, label):
        stats = dict(matched=0, missing=0, mismatched=0, protocol_error=0)
        samples = []
        client = self.client('replica')
        try:
            for key, expected in records:
                try:
                    kind, value = client.command(b'HGET', key)
                    if kind != 'bulk':
                        raise workload.ProtocolError('HGET did not return a bulk reply')
                except workload.ProtocolError:
                    stats['protocol_error'] += 1
                    raise
                if value is None:
                    stats['missing'] += 1
                elif value != expected:
                    stats['mismatched'] += 1
                else:
                    stats['matched'] += 1
                    continue
                if len(samples) < 5:
                    samples.append(key.decode())
        finally:
            client.close()
            self.event('verification', label=label, counts=stats, sample_keys=samples)
        require(stats['matched'] == len(records), f'{label} verification failed')
        return stats

    def catchup(self):
        self.prepare(True)
        payload = b'x' * (64 * 1024 - 32)
        seed = [(f'{self.args.run_id}_seed_{i:04d}'.encode(),
                 f'value {i} '.encode() + payload + b'\r\nsecond line') for i in range(512)]
        during = [(f'{self.args.run_id}_during_{i:02d}'.encode(),
                   f'during {i}\r\nsecond line'.encode()) for i in range(17)]
        after = [(f'{self.args.run_id}_online_{i:02d}'.encode(),
                  f'online {i}\r\nsecond line'.encode()) for i in range(17)]
        writer = self.client('primary')
        try:
            with phase_timeout(self.args.phase_timeout):
                for index, (key, value) in enumerate(seed, 1):
                    self.write_checked(writer, key, value)
                    if index % 64 == 0:
                        self.event('seed_progress', accepted=index, total=len(seed))
            self.start('replica')
            self.wait(lambda: self.gate.ready.is_set(), 'Snapshot header at relay', 30)
            require(self.gate.metadata and self.gate.metadata['size'] > 30 * 1024 * 1024,
                    'Snapshot fixture unexpectedly small')
            require(not self.ranges(), 'Primary completed Snapshot before controlled writes')
            self.event('catchup_writes_start', count=len(during))
            with phase_timeout(self.args.phase_timeout):
                for key, value in during:
                    self.write_checked(writer, key, value)
            self.event('catchup_writes_accepted', accepted=len(during))
            require(not self.ranges(), 'Snapshot hold did not cover all controlled writes')
            self.event('snapshot_released')
            self.gate.release.set()
            self.online()
            ranges = self.ranges()
            require(any(a == self.gate.metadata['offset'] and b > a for a, b in ranges),
                    f'nonempty AOF catch-up not proven: {ranges}')
            for key, value in after:
                self.write_checked(writer, key, value)
            self.event('online_writes_accepted', accepted=len(after))
            # Wait for the last online marker without assuming the ONLINE log means equality.
            key, expected = after[-1]
            def replicated():
                reader = self.client('replica')
                try:
                    return reader.command(b'HGET', key) == ('bulk', expected)
                finally:
                    reader.close()
            self.wait(replicated, 'last ONLINE marker readable from Replica')
            with phase_timeout(self.args.phase_timeout):
                result = dict(seed=self.verify_hash_records(seed, 'Snapshot seed'),
                              catchup=self.verify_hash_records(during, 'held-Snapshot increments'),
                              online=self.verify_hash_records(after, 'ONLINE increments'))
            self.gate.check()
            return dict(scenario='controlled_two_vm_catchup_supplement', success=True,
                        counts=result, catchup_ranges=ranges, snapshot=self.gate.metadata,
                        nonempty_catchup_proven=True, total_verified=546,
                        note='test relay and ~32 MiB fixture; separate from direct 100k acceptance')
        finally:
            writer.close()

    def cleanup(self):
        errors = []
        # Stop Replica, then relay, then Primary. Only owner.json processes are signaled.
        for role, call, info in reversed(self.started):
            try:
                result = call('stop', info)
                self.event('server_stopped', role=role, **result)
                require(result['stopped'] and not result['forced'], f'{role} required forced cleanup or remains alive')
                status = call('status', info)
                require(not status['alive'] and not status['listeners'], f'{role} still owns resources')
            except Exception as exc:
                errors.append(f'stop {role}: {exc}; directory={info["root"]}')
            try:
                text = call('log', info)
                (self.root/f'{role}.log').write_text(text)
            except Exception as exc:
                errors.append(f'collect {role} log: {exc}')
            if role == 'replica' and self.gate:
                try:
                    self.gate.close()
                except Exception as exc:
                    errors.append(f'close relay: {exc}')
                self.gate = None
        if self.gate:
            try:
                self.gate.close()
            except Exception as exc:
                errors.append(f'close relay: {exc}')
            self.gate = None
        self.event('cleanup', success=not errors, errors=errors,
                   note='owned processes stopped; data, configs and evidence retained')
        self.events.close()
        return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', required=True, choices=('direct', 'catchup', 'both'))
    parser.add_argument('--repo', required=True, type=Path)
    parser.add_argument('--primary-host', required=True)
    parser.add_argument('--primary-port', required=True, type=int)
    parser.add_argument('--replica-ssh', required=True, help='SSH alias or user@host')
    parser.add_argument('--ssh-port', type=int, default=22)
    parser.add_argument('--replica-repo', required=True)
    parser.add_argument('--replica-host', required=True)
    parser.add_argument('--replica-port', required=True, type=int)
    parser.add_argument('--artifacts', required=True, type=Path, help='existing local parent directory')
    parser.add_argument('--replica-artifacts', required=True, help='existing remote parent directory')
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--phase-timeout', type=int, default=300)
    args = parser.parse_args()
    require(sys.platform.startswith('linux'), 'run on Linux VM-A, not on the Mac')
    require(args.primary_host != args.replica_host, 'two distinct VM addresses are required')
    for host in (args.primary_host, args.replica_host):
        socket.inet_pton(socket.AF_INET, host)
        require(not host.startswith('127.') and host != '0.0.0.0', 'use reachable VM IPv4 addresses')
    require(not args.replica_ssh.startswith('-'), 'invalid SSH target')
    require(all(1 <= p <= 65535 for p in (args.primary_port, args.replica_port, args.ssh_port)), 'invalid port')
    require(args.phase_timeout > 0, 'phase-timeout must be positive')
    require(args.run_id.isascii() and args.run_id.replace('_', '').isalnum(), 'invalid run-id')
    args.repo = args.repo.resolve()
    root = Path(tempfile.mkdtemp(prefix='91kv-two-vm-', dir=args.artifacts.resolve()))
    print(f'Evidence directory: {root}', flush=True)
    (root/'invocation.json').write_text(json.dumps(dict(
        arguments={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
        utc=utc(), script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        workload_sha256=hashlib.sha256(Path(workload.__file__).read_bytes()).hexdigest()), indent=2))
    remote = None
    results = []
    exit_code = 1
    try:
        remote = Remote(args.replica_ssh, args.ssh_port)
        for mode in (('direct', 'catchup') if args.mode == 'both' else (args.mode,)):
            directory = root/mode
            directory.mkdir()
            suite = Suite(args, directory, remote)
            result = dict(scenario=mode, success=False)
            try:
                result = suite.direct() if mode == 'direct' else suite.catchup()
            except (Exception, KeyboardInterrupt) as exc:
                result['error'] = repr(exc)
                suite.event('failed', error=repr(exc))
            finally:
                errors = suite.cleanup()
                result['cleanup_errors'] = errors
                result['success'] = result.get('success', False) and not errors
                (directory/'summary.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
                results.append(result)
            if not result['success']:
                break
        exit_code = 0 if results and all(r['success'] for r in results) else 1
    except (Exception, KeyboardInterrupt) as exc:
        results.append(dict(success=False, error=repr(exc)))
    finally:
        if remote:
            try:
                remote.close()
            except Exception as exc:
                results.append(dict(success=False, ssh_cleanup_error=repr(exc)))
                exit_code = 1
        (root/'summary.json').write_text(json.dumps(dict(
            exit_code=exit_code, utc=utc(), results=results,
            note='two Linux VMs; physical-network performance is not measured'), ensure_ascii=False, indent=2))
        print(f'[{"PASS" if exit_code == 0 else "FAIL"}] exit={exit_code}; evidence={root}', flush=True)
    return exit_code


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (Exception, KeyboardInterrupt) as exc:
        print(f'[FAIL] {exc}', file=sys.stderr)
        sys.exit(1)
