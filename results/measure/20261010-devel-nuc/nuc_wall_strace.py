#!/usr/bin/env python3
"""Additional wall-clock summaries after the existing CPU-accounting strace."""
import json,subprocess,time
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261010/allocation-profile')
for cell in ('nvme-grow','nvme-ring','sata-grow','sata-ring'):
    folder=base/cell;deadline=time.monotonic()+1200
    while not (folder/'strace-summary.txt.exit').exists():
        assert time.monotonic()<deadline
        time.sleep(.25)
    identity=json.loads((folder/'identity.json').read_text());pid=identity['pid'];proc=Path('/proc',str(pid))
    try:
        command=proc.joinpath('cmdline').read_bytes().decode().split('\0')
        assert command==identity['cmdline']+[''] if identity['cmdline'][-1]!='' else command==identity['cmdline']
    except (FileNotFoundError,ProcessLookupError):
        (folder/'strace-wall-NOTRUN.txt').write_text('The measured process had already exited; no substitute process was attached.\n')
        continue
    with (folder/'strace-wall.txt').open('w') as log:
        rc=subprocess.run(['timeout','--signal=INT','10','strace','-f','-c','-w','-p',str(pid)],stdout=log,stderr=subprocess.STDOUT,timeout=30).returncode
    (folder/'strace-wall-exit.txt').write_text(str(rc)+'\n')
