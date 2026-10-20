#!/usr/bin/env python3
"""Small demo entry point. Personal machine settings live outside Git."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shlex
import sys
import uuid


REQUIRED = ('primary_host', 'primary_port', 'replica_ssh', 'replica_repo',
            'replica_host', 'replica_port', 'artifacts', 'replica_artifacts')
OPTIONAL = ('repo', 'mode', 'ssh_port', 'phase_timeout')


def build_command(config, script_directory, mode=None):
    unknown = set(config) - set(REQUIRED + OPTIONAL)
    if unknown:
        raise ValueError(f'unknown configuration fields: {sorted(unknown)}')
    missing = [key for key in REQUIRED if key not in config]
    if missing:
        raise ValueError(f'missing configuration fields: {missing}')
    for key in REQUIRED + OPTIONAL:
        if key not in config:
            continue
        value = config[key]
        if key in ('primary_port', 'replica_port', 'ssh_port', 'phase_timeout'):
            if type(value) is not int or value < 1:
                raise ValueError(f'{key} must be a positive integer')
            if key != 'phase_timeout' and value > 65535:
                raise ValueError(f'{key} must be at most 65535')
        elif not isinstance(value, str) or not value.strip():
            raise ValueError(f'{key} must be a nonempty string')
    selected_mode = mode or config.get('mode', 'both')
    if selected_mode not in ('direct', 'catchup', 'both'):
        raise ValueError('mode must be direct, catchup or both')
    orchestrator = script_directory/'run_two_vm_acceptance.py'
    if not orchestrator.is_file() or not (script_directory/'test_two_vm_100k.py').is_file():
        raise ValueError('put all three Python scripts in the same tests directory')
    run_id = 'demo_' + datetime.now(timezone.utc).strftime('%Y%m%d_%H%M%S_') + uuid.uuid4().hex[:8]
    command = [sys.executable, '-B', str(orchestrator), '--mode', selected_mode,
               '--repo', config.get('repo', str(script_directory.parent)), '--run-id', run_id]
    for key in REQUIRED + ('ssh_port', 'phase_timeout'):
        if key in config:
            command.extend(['--' + key.replace('_', '-'), str(config[key])])
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path,
                        default=Path.home()/'.config/91kvstore/two-vm-demo.json')
    parser.add_argument('--mode', choices=('direct', 'catchup', 'both'))
    parser.add_argument('--show', action='store_true', help='show command without starting any server')
    args = parser.parse_args()
    if not args.config.is_file():
        parser.error(f'create your personal demo configuration first: {args.config}')
    try:
        config = json.loads(args.config.read_text(encoding='utf-8'))
        if not isinstance(config, dict):
            raise ValueError('configuration must be a JSON object')
        command = build_command(config, Path(__file__).resolve().parent, args.mode)
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    print('Demo config: ' + str(args.config), flush=True)
    print('Running: ' + shlex.join(command), flush=True)
    if not args.show:
        # Replace this process: signals and the final exit status belong to the
        # orchestrator that already handles owned-process cleanup.
        os.execv(sys.executable, command)


if __name__ == '__main__':
    main()
