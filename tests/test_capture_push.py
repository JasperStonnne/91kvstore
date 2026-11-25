"""Nonprivileged active capture client: lost ACK resume, split transfer and cache eviction."""
import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
from test_capture_server import frame

PRIMARY = b'1' * 40
SYNC = b'2' * 40

class Source:
    def __init__(self, root, binary):
        self.root = root
        self.path = root / 'events'
        self.path.touch()
        self.offset = 0
        self.state()
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(4)
        self.listener.settimeout(5)
        self.log = (root / 'capture.log').open('wb')
        self.process = subprocess.Popen([str(binary), str(os.getpid()), str(self.path)],
                                        stdout=self.log, stderr=self.log)
    def state(self):
        path = Path(str(self.path) + '.state')
        temp = self.root / 'state.next'
        temp.write_text(f'2 {PRIMARY.decode()} {SYNC.decode()} 0 {self.offset} 0 0 0 0 0 0\n')
        temp.replace(path)
    def events(self, commands):
        with self.path.open('ab') as stream:
            for data in commands:
                begin = self.offset
                self.offset += len(data)
                stream.write(struct.pack('=IIQQ4096s', 1, len(data), begin, self.offset, data))
        self.state()
    def ready(self):
        with Path(str(self.path) + '.ready').open('ab') as stream:
            stream.write(struct.pack('=IHH16s40s40sQ', 1, self.listener.getsockname()[1], 0,
                                     b'127.0.0.1', PRIMARY, SYNC, 0))
    def accept(self, expected):
        conn, _ = self.listener.accept()
        conn.settimeout(5)
        file = conn.makefile('rb')
        assert file.readline() == b'BEGIN ' + PRIMARY + b' ' + SYNC + f' {expected}\r\n'.encode()
        return conn, file
    def close(self):
        self.process.terminate()
        try: self.process.wait(timeout=5)
        except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        self.log.close()
        self.listener.close()
        assert self.process.returncode == 0

def data(file):
    header = file.readline().split()
    assert len(header) == 3 and header[0] == b'DATA', header
    payload = file.read(int(header[2]))
    assert len(payload) == int(header[2])
    return int(header[1]), payload

def resume(root, binary):
    source = Source(root, binary)
    try:
        first, second, third = [frame('HSET', f'key{i}', 'x') for i in range(3)]
        source.events([first, second]); source.ready()
        conn, file = source.accept(0)
        conn.sendall(b'APPLIED 0\r\n')
        assert data(file) == (0, first + second)
        # Replica applied first, but the APPLIED reply was lost.
        file.close(); conn.close()
        conn, file = source.accept(0)
        conn.sendall(f'APPLIED {len(first)}\r\n'.encode())
        assert data(file) == (len(first), second)
        total = len(first + second)
        conn.sendall(f'APPLIED {total}\r\n'.encode())
        assert file.readline() == f'ONLINE {total}\r\n'.encode()
        source.events([third])
        header = file.readline().split()
        assert header[0] == b'DATA'
        assert file.read(2) == third[:2]
        file.close(); conn.close()  # No complete command was applied.
        conn, file = source.accept(total)
        conn.sendall(f'APPLIED {total}\r\n'.encode())
        assert data(file) == (total, third)
        total += len(third)
        conn.sendall(f'APPLIED {total}\r\n'.encode())
        assert file.readline() == f'ONLINE {total}\r\n'.encode()
        file.close(); conn.close()
        print('[PASS] push client: proactive BEGIN, lost-ACK reconciliation, no duplicate command, truncated DATA replay, ONLINE', flush=True)
    finally: source.close()

def eviction(root, binary):
    source = Source(root, binary)
    try:
        payload = frame('HSET', 'overflow', 'x' * 3900)
        source.events([payload] * 2200)
        deadline = time.monotonic() + 8
        while True:
            text = (root / 'capture.log').read_text()
            if 'cache_start=' in text and any('cache_start=0 ' not in line and 'cache_start=' in line for line in text.splitlines()):
                break
            assert time.monotonic() < deadline, text[-1000:]
            time.sleep(.02)
        source.ready()
        conn, file = source.accept(0)
        conn.sendall(b'APPLIED 0\r\n')
        assert file.readline().startswith(b'ERROR ')
        file.close(); conn.close()
        print('[PASS] push client: ready event independent of command backlog; stale resume outside 8 MiB cache returns ERROR', flush=True)
    finally: source.close()

def invalid_state(root, binary):
    source = Source(root, binary)
    try:
        Path(str(source.path) + '.state').write_text('invalid state\n')
        time.sleep(.05)
        source.ready()
        source.listener.settimeout(.25)
        try:
            connection, _ = source.listener.accept()
            connection.close()
            raise AssertionError('connected before kernel boundary/state validation')
        except socket.timeout:
            pass
        source.state()
        source.listener.settimeout(5)
        conn, file = source.accept(0)
        conn.sendall(b'APPLIED 0\r\n')
        assert file.readline() == b'ONLINE 0\r\n'
        file.close(); conn.close()
        print('[PASS] push client: invalid boundary/state prevents connect; validated state releases pending readiness', flush=True)
    finally: source.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('repo', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='91kv-push-') as directory:
        for name, case in [('resume', resume), ('eviction', eviction), ('invalid-state', invalid_state)]:
            root = Path(directory) / name; root.mkdir()
            case(root, (args.repo / 'bin/test_capture_mock').resolve())
if __name__ == '__main__': main()
