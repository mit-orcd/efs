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
        # Port 1 is never bound: refusal must precede listen and the store
        # (D39: one member of a configured list; nothing initialized).
        Path(f'{root}/cluster.conf').write_text('1 127.0.0.1:1\n2 127.0.0.2:1\n3 127.0.0.3:1\n')
        p = subprocess.run([str(efsd), '--cluster', f'{root}/cluster.conf', '--addr', '127.0.0.1',
                            '--port', '1', '--storage', root, '--quota', '1G'],
                           capture_output=True, text=True, timeout=30)
        assert p.returncode == 1, (marker, p.returncode, p.stderr[-400:])
        assert f'holds unsupported state {marker}; refusing' in p.stderr, (marker, p.stderr[-400:])
        assert 'listening' not in p.stdout + p.stderr, marker
        assert not os.path.exists(f'{root}/mdraft'), (marker, 'no member store was initialized')
print('retired root markers: efsd refuses repair-active/-store/-job roots before serving PASS')
