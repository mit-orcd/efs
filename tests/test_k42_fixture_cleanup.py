#!/usr/bin/env python3
"""A failed K42 unmount remains failed and closes only its synthetic peer."""
import ast
import os
import tempfile
import threading
import time
from pathlib import Path
from types import SimpleNamespace

root = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('EFS_K42_SOURCE', root/'tests/test_k42_peer_stop_open.py'))
tree = ast.parse(source.read_text())
fn = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'peer_round')

with tempfile.TemporaryDirectory(prefix='efs-k42-cleanup-') as tmp:
    path = Path(tmp)
    closed = []
    peer = SimpleNamespace(path=path, p=SimpleNamespace(poll=lambda: None),
                           close=lambda: closed.append('peer'))
    def unmount(m):
        assert m is peer
        raise RuntimeError('injected unmount BUSY')
    def append(p, text):
        with p.open('a') as f:
            f.write(text)
    names = dict(Mount=lambda *a: peer, append=append, threading=threading,
                 time=time, unmount=unmount)
    exec(compile(ast.Module(body=[fn], type_ignores=[]), str(source), 'exec'), names)
    hold = path/'hold'
    try:
        names['peer_round'](None, SimpleNamespace(path=path), hold, 'peer', 3, cleanup=True)
    except RuntimeError as e:
        assert str(e) == 'injected unmount BUSY'
    else:
        raise AssertionError('the failed unmount was hidden')
    assert not hold.exists(), 'the synthetic REPORT hold was stranded'
    assert closed == ['peer'], 'the synthetic peer was stranded or another client was closed'
print('K42 cleanup: original unmount error preserved, hold removed and exact peer closed PASS')
