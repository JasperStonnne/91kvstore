#!/usr/bin/env python3
"""Run as the ordinary login user. Only the build writes ignored build products."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
HEAD = '49b8d3d0ce1f8f9895900fb357c431c1d9697752'
BASE = Path('/tmp/91kv-stage44b-manual')
FILES = ['common.py', 'agent.py', 'controller.py', 'client.c', 'summarize.py', 'prepare.py', 'selftest.py']


def run(*args, **kwargs):
    return subprocess.check_output(list(map(str, args)), text=True, **kwargs).strip()


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def primary(repo):
    repo = repo.resolve()
    assert os.geteuid() != 0, 'Prepare as jaspersao, not sudo'
    assert run('git', '-C', repo, 'rev-parse', 'HEAD') == HEAD, 'HEAD must be 49b8d3d'
    assert not run('git', '-C', repo, 'status', '--porcelain'), 'Tracked/untracked changes exist; inspect first'
    if BASE.exists():
        allowed=set(FILES)|{'build.log','client','payload'}
        assert all(p.name in allowed for p in BASE.iterdir()), 'Existing directory contains a plan/results; preserve it first'
    BASE.mkdir(mode=0o700,exist_ok=True)
    for name in FILES:
        shutil.copy2(HERE/name, BASE/name)
    # Force first-party rebuild; retain exact output, including third-party flags.
    command = ['make', '-B', 'OPT=-O2', 'bin/kvstore', 'bin/kvstore-ebpf-capture']
    with (BASE/'build.log').open('w') as log:
        log.write('$ ' + ' '.join(command) + '\n');log.flush()
        subprocess.run(command, cwd=repo, stdout=log, stderr=log, check=True)
    warnings = [line for line in (BASE/'build.log').read_text(errors='replace').splitlines() if 'warning:' in line]
    assert not warnings, 'Build warnings: inspect '+str(BASE/'build.log')
    assert not run('git', '-C', repo, 'status', '--porcelain'), 'Build changed nonignored repository files'
    subprocess.run(['gcc', '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(BASE/'client.c'), '-o', str(BASE/'client')], check=True)
    subprocess.run(['python3','-B',str(BASE/'selftest.py'),str(BASE/'client')],check=True)
    binary = repo/'bin/kvstore'
    capture = repo/'bin/kvstore-ebpf-capture'
    bpf = repo/'build/ebpf_capture.bpf.o'
    assert binary.is_file() and capture.is_file() and bpf.is_file()
    token = secrets.token_hex(32)
    payload = BASE/'payload';payload.mkdir(exist_ok=True)
    for name in FILES:
        shutil.copy2(BASE/name, payload/name)
    subprocess.run(['git', '-C', str(repo), 'bundle', 'create', str(payload/'repo.bundle'), 'main'], check=True)
    assert run('git', '-C', repo, 'rev-parse', 'main') == HEAD
    sub = repo/'third_party/NtyCo'
    assert not [line for line in run('git', '-C', sub, 'status', '--porcelain').splitlines() if line != '?? libntyco.a'], 'NtyCo dirty (excluding generated libntyco.a)'
    subprocess.run(['git', '-C', str(sub), 'bundle', 'create', str(payload/'ntyco.bundle'), '--all', 'HEAD'], check=True)
    shutil.copy2(binary, payload/'kvstore')
    agent = dict(repo=str(BASE/'repo'), host='172.16.145.136', primary_host='172.16.145.135',
                 agent_port=29444, service_port=29192, primary_port=29191, head=HEAD,
                 binary_sha256=digest(binary), token=token,
                 submodule_head=run('git', '-C', sub, 'rev-parse', 'HEAD'))
    (payload/'agent-plan.json').write_text(json.dumps(agent, indent=2))
    plan = dict(primary_repo=str(repo), primary_binary=str(binary), capture_binary=str(capture),
                bpf_object=str(bpf), client=str(BASE/'client'), primary_host=agent['primary_host'],
                replica_host=agent['host'], agent_port=29444, primary_port=29191,
                replica_port=29192, rdma_port=29747, head=HEAD, binary_sha256=digest(binary),
                token=token, uid=os.getuid(), gid=os.getgid(), requests=100000, connections=8, pipeline=1)
    paths=[binary,capture,bpf,BASE/'client',BASE/'controller.py',BASE/'common.py',BASE/'summarize.py']
    plan['expected_files']={str(p):digest(p) for p in paths}
    (BASE/'plan.json').write_text(json.dumps(plan, indent=2));(BASE/'plan.json').chmod(0o600)
    with tarfile.open(BASE/'to136.tar.gz', 'w:gz') as tar:
        for path in payload.iterdir():tar.add(path, arcname=path.name)
    (BASE/'to136.tar.gz').chmod(0o600)
    shutil.rmtree(payload)
    print('PREPARED 135: HEAD',HEAD,'ELF',digest(binary),'build warnings=0')
    print('Transfer',BASE/'to136.tar.gz','to 136 and follow README step 3.')


def replica():
    assert os.geteuid() != 0, 'Prepare as jaspersao, not sudo'
    assert HERE == BASE, f'Extract to {BASE}'
    plan=json.loads((BASE/'agent-plan.json').read_text())
    assert not (BASE/'repo').exists(), 'Replica checkout already exists'
    run('git','clone',str(BASE/'repo.bundle'),str(BASE/'repo'))
    run('git','-C',BASE/'repo','checkout','--detach',HEAD)
    run('git','clone',str(BASE/'ntyco.bundle'),str(BASE/'ntyco-source'))
    run('git','-C',BASE/'ntyco-source','checkout','--detach',plan['submodule_head'])
    run('git','-C',BASE/'repo','-c','protocol.file.allow=always',
        '-c',f'submodule.NtyCo.url={BASE}/ntyco-source','submodule','update','--init')
    (BASE/'repo/bin').mkdir(exist_ok=True)
    shutil.copy2(BASE/'kvstore',BASE/'repo/bin/kvstore');(BASE/'repo/bin/kvstore').chmod(0o755)
    assert digest(BASE/'repo/bin/kvstore')==plan['binary_sha256']
    assert not run('git','-C',BASE/'repo','status','--porcelain')
    for command in ['pidstat','perf','stdbuf']:
        assert shutil.which(command), f'Missing {command}; install before running agent'
    (BASE/'agent-plan.json').chmod(0o600)
    print('PREPARED 136: isolated clean HEAD',HEAD,'identical ELF',plan['binary_sha256'])
    print('Start: sudo python3 -B -u /tmp/91kv-stage44b-manual/agent.py')


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('side',choices=['primary','replica'])
    parser.add_argument('--repo',type=Path,default=Path('/home/jaspersao/dev/91kvstore'))
    args=parser.parse_args()
    if args.side=='primary':primary(args.repo)
    else:replica()
