#!/usr/bin/env python3
"""Task C deterministic byte mutations, including caller-state invariance."""
from pathlib import Path
import subprocess
import os
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
TARGETS = ('membership', 'member-view', 'discovery-probe', 'discovery-answer',
           'boot', 'identity', 'fragment', 'fragment-verify', 'ticket', 'ticket-batch', 'ticket-settle',
           'hello', 'recovery-request', 'recovery-reply', 'audit-request', 'audit-reply',
           'degraded', 'degraded-command', 'recovery-command', 'pack-degraded', 'ticket-drain', 'put-header', 'cleanup-probe', 'receipt', 'declaration')
def source_copy_regression():
    sys.path.insert(0,str(ROOT/'scripts'))
    from decoder_source import copy_source
    with tempfile.TemporaryDirectory(prefix='efs-decoder-copy-') as d:
        source=Path(d)/'source';source.mkdir()
        (source/'Makefile').write_text('all:\n')
        (source/'src').mkdir();(source/'src/a.c').write_text('/* source */\n')
        (source/'tmp').mkdir();os.mkfifo(source/'tmp/GMfifo')
        (source/'src/tmp').mkdir();os.mkfifo(source/'src/tmp/active-fifo')
        (source/'src/link').symlink_to('/does-not-exist')
        (source/'ACCOUNTS.txt').write_text('must never be copied')
        copy_source(source,Path(d)/'build')
        assert (Path(d)/'build/src/a.c').read_text()=='/* source */\n'
        assert not (Path(d)/'build/tmp').exists()
        assert not (Path(d)/'build/src/tmp').exists()
        assert not (Path(d)/'build/src/link').is_symlink()
        assert not (Path(d)/'build/ACCOUNTS.txt').exists()
        try:copy_source(source,source/'src/build')
        except ValueError:pass
        else:raise AssertionError('recursive destination accepted')
    print('PASS decoder source-copy excludes active pipes, runtime trees, symlinks and private account files')

if __name__ == '__main__':
    source_copy_regression()
    subprocess.run([str(ROOT/'tests/decoders/apply')], check=True)
    for target in TARGETS:
        subprocess.run([str(ROOT/'tests/decoders/check'), target], check=True)
