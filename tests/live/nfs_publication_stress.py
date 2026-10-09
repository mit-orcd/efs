#!/usr/bin/env python3
"""W55 byte oracle for NFS copies, overwrites, truncation and name reuse.
No fsync is added: the observed boundary is successful cp/close. Retain the
ledger and audit every final inode from an independent gateway/server view.
macOS uses explicit cp -X to separate byte publication from xattr compatibility.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('directory', type=Path)
p.add_argument('--ledger', required=True, type=Path)
p.add_argument('--workers', type=int, default=8)
p.add_argument('--iterations', type=int, default=20)
a = p.parse_args()
if a.workers < 1 or a.iterations < 1:
    p.error('workers and iterations must be positive')
a.directory.mkdir()  # a fresh, exclusively owned directory
sources = a.ledger.parent / (a.ledger.stem + '-sources')
sources.mkdir()
started = time.time()

def worker(w):
    records, errors, passed = {}, [], 0
    source = sources / str(w)
    for i in range(a.iterations):
        # Small original-shape body, then block-boundary and multi-page tails.
        size = (2922, 4095, 4097, 131073)[i % 4]
        prefix = ('worker=%d iteration=%d\n' % (w, i)).encode()
        body = (prefix + bytes(range(256)) * ((size // 256) + 1))[:size]
        source.write_bytes(body)
        for mode in ('fresh', 'overwrite', 'truncate', 'reuse'):
            name = '%s-%02d-%03d' % (mode, w, i) if mode == 'fresh' else '%s-%02d' % (mode, w)
            path = a.directory / name
            try:
                if mode == 'reuse' and path.exists():
                    path.unlink()
                    records.pop(name, None)
                if mode == 'truncate':
                    # Explicit truncate/rewrite, distinct from cp overwrite.
                    with path.open('wb') as f:
                        f.write(body)
                else:
                    command = ['cp'] + (['-X'] if sys.platform == 'darwin' else [])
                    r = subprocess.run(command + [str(source), str(path)],
                                       capture_output=True, text=True, timeout=60)
                    if r.returncode:
                        raise RuntimeError('cp rc=%d stderr=%s' % (r.returncode, r.stderr))
                data = path.read_bytes()
                if data != body:
                    raise RuntimeError('warm bytes differ: length=%d' % len(data))
                records[name] = {'name':name, 'inode':path.stat().st_ino,
                    'size':size, 'sha256':hashlib.sha256(body).hexdigest(),
                    'worker':w, 'iteration':i, 'mode':mode}
                passed += 1
            except Exception as e:
                errors.append({'name':name, 'iteration':i, 'error':repr(e)})
                # Preserve failures, avoid overwriting their namespace evidence.
                return records, errors, passed
    return records, errors, passed

records, errors, passed = {}, [], 0
with concurrent.futures.ThreadPoolExecutor(max_workers=a.workers) as pool:
    for recs, errs, count in pool.map(worker, range(a.workers)):
        records.update(recs); errors.extend(errs); passed += count
ledger = {'platform':sys.platform, 'directory':str(a.directory),
    'workers':a.workers, 'iterations':a.iterations,
    'boundary':'successful cp/close, no added fsync',
    'copy_command':'cp -X' if sys.platform == 'darwin' else 'cp',
    'operations_passed':passed, 'operations_expected':a.workers*a.iterations*4,
    'elapsed_seconds':time.time()-started, 'files':list(records.values()),
    'errors':errors}
a.ledger.write_text(json.dumps(ledger, indent=2)+'\n')
print(json.dumps({k:v for k,v in ledger.items() if k != 'files'}), flush=True)
sys.exit(bool(errors))
