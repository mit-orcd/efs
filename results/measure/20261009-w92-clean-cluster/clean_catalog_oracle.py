#!/usr/bin/env python3
"""Caller-owned clean-cluster tree: independent model vs .efs disk queries."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

phase, mount_text, model_text = sys.argv[1:]
mount, model_path = Path(mount_text), Path(model_text)
root = mount / 'w92-clean-catalog'

def command(name, *args):
    result = subprocess.run([sys.executable, str(root / '.efs' / name), *args],
                            capture_output=True, timeout=45)
    if result.returncode:
        raise AssertionError((name, result.returncode, result.stderr.decode(errors='replace')))
    return json.loads(result.stdout)

def check(model):
    deadline = time.monotonic() + 90
    while True:
        try:
            stats = command('stats', '--json')['stats']
            paths = command('find', '.dat', '--match', 'suffix', '--limit', '10000', '--json')
            actual = {bytes.fromhex(entry['path_hex']).decode() for entry in paths['entries']}
            assert actual == set(model), (len(actual), len(model), sorted(actual ^ set(model))[:5])
            assert stats['tree_files'] == len(model), stats
            assert stats['tree_dirs'] == 16, stats
            assert stats['tree_bytes'] == sum(item['size'] for item in model.values()), stats
            break
        except (AssertionError, OSError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(1)
    for path in sorted(model)[::max(1, len(model)//64)]:
        payload = (root / path).read_bytes()
        assert len(payload) == model[path]['size']
        assert hashlib.sha256(payload).hexdigest() == model[path]['sha256'], path
    print(json.dumps({'phase': phase, 'files': len(model), 'dirs': 16,
                      'logical_bytes': sum(item['size'] for item in model.values()),
                      'snapshot': paths['snapshot'], 'result': 'PASS'}), flush=True)

if phase == 'create':
    root.mkdir()
    for directory in range(16):
        (root / ('d%02d' % directory)).mkdir()
    def create(index):
        path = 'd%02d/file-%05d.dat' % (index % 16, index)
        payload = bytes([index % 251]) * 32
        (root / path).write_bytes(payload)
        return path, {'size': len(payload), 'sha256': hashlib.sha256(payload).hexdigest()}
    start = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        model = dict(pool.map(create, range(1500)))
    model_path.write_text(json.dumps(model, sort_keys=True))
    print('create_seconds=%.3f' % (time.monotonic()-start), flush=True)
    check(model)
elif phase == 'mutate':
    model = json.loads(model_path.read_text())
    initial = sorted(model)
    for path in initial[:75]:
        (root / path).unlink();del model[path]
    for index, path in enumerate(initial[75:150]):
        new = 'd%02d/renamed-%05d.dat' % ((index+1) % 16, index)
        (root / path).rename(root / new);model[new] = model.pop(path)
    for path in sorted(model)[:15]:
        with (root / path).open('ab') as stream:
            stream.write(b'additional-bytes!')
        payload = (root / path).read_bytes()
        model[path] = {'size': len(payload), 'sha256': hashlib.sha256(payload).hexdigest()}
    model_path.write_text(json.dumps(model, sort_keys=True))
    check(model)
elif phase == 'check':
    check(json.loads(model_path.read_text()))
else:
    raise SystemExit('phase must be create, mutate or check')
