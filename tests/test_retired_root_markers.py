#!/usr/bin/env python3
"""A root left by the retired repair experiment may hold a target write this
build cannot finish or collect: efsd must refuse it before serving."""
import os, subprocess, sys, tempfile
from pathlib import Path

efsd = Path(__file__).resolve().parents[1] / 'efsd'
for marker in ('repair-active', 'repair-store', 'repair-job'):
    with tempfile.TemporaryDirectory(prefix='efs-retired-marker-') as root:
        os.makedirs(f'{root}/meta')
        Path(f'{root}/meta/{marker}').write_bytes(b'')
        # Port 1 is never bound: refusal must precede listen.
        p = subprocess.run([str(efsd), '127.0.0.1:1', root, '--quota', '1G',
                            '--discovery-port', '0'],
                           capture_output=True, text=True, timeout=30)
        assert p.returncode == 1, (marker, p.returncode, p.stderr[-400:])
        assert f'holds unsupported state {marker}; refusing' in p.stderr, (marker, p.stderr[-400:])
        assert 'listening' not in p.stdout + p.stderr, marker
print('retired root markers: efsd refuses repair-active/-store/-job roots before serving PASS')
