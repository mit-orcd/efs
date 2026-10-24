#!/usr/bin/env python3
"""A6: a removed setting that is still set stops efsd and efs-fuse.

The table is removed_env[] in src/common/common.c (efs_env_removed_refuse).
For every row, ./efsd and ./efs-fuse started with the variable set exit 1
before they serve or mount, naming it as removed; EFS_MD_RAFT_N keeps its
W153 wording and an empty value is refused too. A clean environment passes
the gate (each binary then stops on a bad argument instead). Removed names are read
nowhere else (scripts/env_inventory.py --check enforces that).

Usage: make efsd efs-fuse && python3 tests/test_env_removed.py"""
from pathlib import Path
import os, re, shutil, socket, subprocess, sys, tempfile
root = Path(__file__).resolve().parents[1]
efsd, fuse = root / 'efsd', root / 'efs-fuse'
for b in (efsd, fuse):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
src = (root / 'src/common/common.c').read_text()
table = src[src.index('removed_env[] = {'):]
table = table[:table.index('\n};')]
names = re.findall(r'\{ "(EFS_[A-Z0-9_]+)"', table)
assert len(names) >= 19 and 'EFS_MD_RAFT_N' in names and 'EFS_GC_DISABLE' in names, names
base = {k: v for k, v in os.environ.items() if not k.startswith('EFS_')}


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


work = Path(tempfile.mkdtemp(prefix='efs-env-removed-', dir=os.environ.get('TMPDIR')))
try:
    mnt = work / 'mnt'
    mnt.mkdir()
    for name in names:
        for value in (('1', '') if name == 'EFS_MD_RAFT_N' else ('1',)):
            env = dict(base, **{name: value})
            for argv, prog in (([str(efsd), '127.0.0.1:%d' % port(), str(work / 'store')], 'efsd'),
                               ([str(fuse), '127.0.0.1:1', str(mnt)], 'efs-fuse')):
                p = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=20)
                assert p.returncode == 1, (name, prog, p.returncode, p.stderr[-600:])
                assert f'{prog}: {name} was removed (' in p.stderr, (name, prog, p.stderr[-600:])
                assert 'Unset it and start again.' in p.stderr, (name, prog, p.stderr[-600:])
                assert 'raft-host: up' not in p.stderr and 'starting export' not in p.stderr, p.stderr[-600:]
            assert not (work / 'store').exists(), name
    p = subprocess.run([str(efsd), '127.0.0.1:1', str(work / 'x')], env=dict(base, EFS_MD_RAFT_N='3'),
                       capture_output=True, text=True, timeout=20)
    assert 'EFS_MD_RAFT_N was removed (W153): the member count' in p.stderr, p.stderr
    # Two removed names: both named, one exit.
    p = subprocess.run([str(fuse), '127.0.0.1:1', str(mnt)],
                       env=dict(base, EFS_META_BATCH_OPS='8', EFS_FEATURES_TTL_MS='5'),
                       capture_output=True, text=True, timeout=20)
    assert p.returncode == 1 and p.stderr.count(' was removed (') == 2, p.stderr
    # Control: with nothing removed set, both pass the gate and stop later
    # on a bad argument instead.
    p = subprocess.run([str(efsd), '--no-such-flag'], env=base, capture_output=True, text=True, timeout=20)
    assert p.returncode == 1 and 'Unknown argument: --no-such-flag' in p.stderr, p.stderr
    assert ' was removed (' not in p.stderr, p.stderr
    p = subprocess.run([str(fuse), '127.0.0.1:1', '127.0.0.2:1'], env=base, capture_output=True, text=True, timeout=20)
    assert p.returncode == 1 and 'Missing mountpoint' in p.stderr and ' was removed (' not in p.stderr, p.stderr
finally:
    shutil.rmtree(work, ignore_errors=True)
print('A6 removed settings: %d names refused by efsd and efs-fuse at startup; clean environment passes the gate PASS'
      % len(names))
