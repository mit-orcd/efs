#!/usr/bin/env python3
"""A full pending probe window refuses new work without losing its owners."""
from pathlib import Path
import os, shlex, subprocess, tempfile
from sanitize_env import cflags, timeout, wrap
root=Path(__file__).resolve().parents[1]
source=(root/'tests/review_graph_20261028/receipt_pending.c').read_text()
source=source.replace('"../../src/common/request_receipt.c"', '"'+str(root/'src/common/request_receipt.c')+'"')
source=source.replace('return fixed ? (present && peak_first == 1 && duplicate.rc == EFS_ERR_BUSY ? 0 : 1)',
    'return fixed ? (present && peak_first == 1 && duplicate.rc == EFS_ERR_BUSY && ninth.rc == EFS_ERR_BUSY && !ninth.entered && ninth.how != EFS_RECEIPT_RAN ? 0 : 1)')
with tempfile.TemporaryDirectory(prefix='efs-receipt-pending-') as temp:
    p=Path(temp)/'check.c';p.write_text(source)
    cc=shlex.split(os.environ.get('CC','cc'))
    subprocess.run(cc+['-std=gnu11','-Wall','-Wextra','-Werror','-g','-I'+str(root/'include'),*cflags(),str(p),'-pthread','-o',str(p.with_suffix(''))],check=True)
    subprocess.run(wrap()+[str(p.with_suffix('')),'fixed'],check=True,timeout=timeout(30))
print('receipt pressure: pending ownership retained, new work BUSY, duplicate joins without execution PASS')
