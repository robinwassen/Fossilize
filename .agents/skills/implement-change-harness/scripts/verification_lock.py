#!/usr/bin/env python3
"""One-host blocking build/verification lock; child timeout starts after acquisition."""
import argparse
from contextlib import contextmanager
import fcntl
import json
from pathlib import Path
import subprocess
import time


def lock_path(repo):
    common = subprocess.check_output(['git', '-C', str(repo), 'rev-parse',
        '--path-format=absolute', '--git-common-dir'], text=True, timeout=60).strip()
    return Path(common).resolve() / 'harness-verification.lock'


@contextmanager
def file_lock(path, blocking=True):
    # Permanent inode. Try once first so uncontended acquisitions credit no time.
    with Path(path).open('a+') as handle:
        waited = 0.0
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            if not blocking:
                raise
            started = time.monotonic()
            fcntl.flock(handle, fcntl.LOCK_EX)
            waited = time.monotonic() - started
        try:
            yield waited
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


@contextmanager
def verification_lock(repo):
    with file_lock(lock_path(repo)) as waited:
        yield waited


def run(repo, command, timeout, receipt=None, intent=None, cwd=None):
    if not 0 < timeout <= 600:
        raise ValueError('child timeout must be in (0, 600] seconds')
    started = time.time()
    with verification_lock(repo) as waited:
        acquired = time.time()
        try:
            status = subprocess.run(command, cwd=cwd or repo, timeout=timeout).returncode
            if status < 0:
                status = 128 - status
            timed_out = False
        except subprocess.TimeoutExpired:
            status, timed_out = 124, True
        if receipt:
            Path(receipt).write_text(json.dumps({'intent_id': intent, 'command': command,
                'cwd': str(Path(cwd or repo).resolve()), 'timeout': timeout,
                'started': started, 'acquired': acquired, 'wait_seconds': waited,
                'finished': time.time(), 'exit_code': status, 'timed_out': timed_out}, indent=2) + '\n')
        return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', default='.')
    parser.add_argument('--timeout', type=float, default=600)
    parser.add_argument('--receipt', help='fresh private JSON output for coordinator command-wait accounting')
    parser.add_argument('--intent-id')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a child command is required after --')
    if args.receipt and Path(args.receipt).exists():
        parser.error('receipt output already exists; use a fresh path')
    raise SystemExit(run(args.repo, command, args.timeout, args.receipt, args.intent_id))


if __name__ == '__main__':
    main()
