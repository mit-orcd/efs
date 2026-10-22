#!/usr/bin/env python3
"""Exercise the live fork/read oracle, including corrupt/short/long negatives.

Linux + C11 compiler + libcrypto runtime; no EFS mount or root required.
CC may include sanitizer flags. Fixtures and binaries live in a temporary dir.
"""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

if not sys.platform.startswith('linux'):
    print('NOTRUN: live direct-read/fork oracle requires Linux libcrypto runtime')
    raise SystemExit(0)

repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='efs-fork-oracle-') as tmp:
    root = Path(tmp)
    binary = root / 'read-fork'
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-D_DEFAULT_SOURCE', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-pthread', str(repo / 'tests/live/efs_direct_read_fork.c'), '-ldl',
        '-o', str(binary)], check=True)
    fixture = root / 'fixture'
    payloads = {}
    for w in range(8):
        d = fixture / f'worker-{w:02d}' / 'dir-000000'
        d.mkdir(parents=True)
        for off in range(4):
            index = 4*w + off
            data = hashlib.sha256(b'W92-real-payload:' + index.to_bytes(8, 'big')).digest()*8192
            path = d / f'file-{index:09d}'
            path.write_bytes(data)
            payloads[path] = data

    def run(expected, *, fork=False, cow=False, fill=0, rounds=2, repeats=1, args=None, preload=None):
        env = os.environ.copy()
        for key in ('EFS_DIAG_FORK', 'EFS_DIAG_COW'):
            env.pop(key, None)
        if preload:
            env['LD_PRELOAD'] = str(preload)
        if fork:
            env['EFS_DIAG_FORK'] = '1'
        if cow:
            env['EFS_DIAG_COW'] = '1'
        cmd = [str(binary), str(fixture)] + (args if args is not None else [str(fill), '4', '4', str(rounds), str(repeats)])
        result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=60)
        assert result.returncode == expected, (cmd, result.returncode, result.stdout, result.stderr)
        if expected == 0:
            assert f'PASS files={32*rounds*repeats} expected={32*rounds*repeats}' in result.stdout, result.stdout
        print(f'expected_rc={expected} actual_rc={result.returncode}: ' +
              (result.stdout.strip() or result.stderr.strip()))
        return result

    run(0)
    run(0, fill=1)
    run(0, fork=True, rounds=100)
    run(0, fork=True, cow=True, rounds=100)
    run(0, fork=True, cow=True, fill=1, rounds=2, repeats=100)
    for path, data in payloads.items():
        assert path.read_bytes() == data, path
    path = next(iter(payloads))
    good = payloads[path]
    for bad in (b'!' + good[1:], good[:-1], good+b'!'):
        path.write_bytes(bad)
        run(1)
    path.write_bytes(good)
    # Fork exhaustion must fail qualification rather than silently exercising
    # only reads. Native interposer affects this subprocess only; sanitizer
    # runs avoid changing sanitizer loader order.
    if 'sanitize' not in os.environ.get('CC', ''):
        interposer = root / 'fork_fail.c'
        interposer.write_text('#include <sys/types.h>\n#include <errno.h>\n'
                              'pid_t fork(void){errno=EAGAIN;return -1;}\n')
        library = root / 'fork_fail.so'
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
            '-shared', '-fPIC', str(interposer), '-o', str(library)], check=True)
        result = run(1, fork=True, rounds=100, preload=library)
        assert 'forks=0 ' in result.stdout and 'fork:' in result.stderr, result
    run(2, cow=True)
    for args in (['2', '4', '4'], ['0', '0', '4'], ['0', '4', '3'],
                 ['0', '4', '4', '0'], ['0', '4', '4', '1', '0'], ['4294967296', '4', '4'], ['0', '-1', '4']):
        run(2, args=args)
print('PASS: fork activity, disjoint COW stress, exact bytes/EOF, and negative controls')
