#!/usr/bin/env python3
"""Task-I host context every 10 seconds; read-only, never signals processes."""
import json
import os
from pathlib import Path
import sys
import time

source, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
with output.open('x') as log:
    while True:
        started = time.time()
        foreign = []
        for proc in Path('/proc').glob('[0-9]*'):
            try:
                if proc.stat().st_uid != os.getuid():
                    continue
                comm = (proc / 'comm').read_text().strip()
                args = (proc / 'cmdline').read_bytes().decode(errors='replace').split('\0')
                runner = len(args) > 1 and Path(args[1]).name == 'make_test_summary.sh'
                if comm not in ('make', 'gmake', 'efsd', 'efs-fuse', 'gcc', 'cc1') and not runner:
                    continue
                if any(str(source) in x for x in args) or (proc / 'cwd').resolve().is_relative_to(source):
                    continue
                foreign.append(dict(pid=int(proc.name), comm=comm, command=args[:3]))
            except (OSError, ValueError):
                continue
        record = dict(t=started, foreign_jobs=foreign,
                      cpu=Path('/proc/stat').read_text().splitlines()[0],
                      memory=Path('/proc/meminfo').read_text(),
                      disk=Path('/proc/diskstats').read_text(),
                      load=Path('/proc/loadavg').read_text().strip())
        log.write(json.dumps(record) + '\n')
        log.flush()
        time.sleep(max(0.1, 10 - (time.time() - started)))
