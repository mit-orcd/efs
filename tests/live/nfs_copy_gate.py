#!/usr/bin/env python3
"""W55 cp/close acceptance ledger; run prepare, cold remount, then verify.
Run on macOS against its NFS mount for original-shape acceptance. Linux NFS
results are useful additional evidence but cannot substitute for that gate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('phase', choices=('prepare', 'verify'))
p.add_argument('directory', type=Path)
p.add_argument('--ledger', type=Path, required=True)
p.add_argument('--source', type=Path, help='existing source file; default deterministic 2922-byte body')
a = p.parse_args()
if a.phase == 'prepare':
    a.directory.mkdir()  # refuse existing names or previous contaminated runs
    source = a.source
    if source is None:
        source = a.ledger.with_suffix('.source')
        source.write_bytes((bytes(range(256)) * 12)[:2922])
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    files = []
    for index in range(20):
        path = a.directory / f'copy-{index:02d}'
        subprocess.run(['cp', str(source), str(path)], check=True)
        assert path.read_bytes() == data, path
        files.append({'name': path.name, 'ino': path.stat().st_ino})
    a.ledger.write_text(json.dumps({'platform': sys.platform,
        'directory': str(a.directory), 'bytes_per_file': len(data),
        'sha256': digest, 'boundary': 'successful cp/close; no added fsync',
        'files': files, 'warm_verified': 20}, indent=2))
    print('20/20 cp and warm exact bytes PASS; cold restart/remount still required')
else:
    ledger = json.loads(a.ledger.read_text())
    assert str(a.directory) == ledger['directory']
    for record in ledger['files']:
        path = a.directory / record['name']
        data = path.read_bytes()
        assert len(data) == ledger['bytes_per_file'], path
        assert hashlib.sha256(data).hexdigest() == ledger['sha256'], path
        assert path.stat().st_ino == record['ino'], path
    ledger['cold_verified'] = len(ledger['files'])
    a.ledger.write_text(json.dumps(ledger, indent=2))
    print('20/20 cp cold exact bytes and inode identity PASS')
