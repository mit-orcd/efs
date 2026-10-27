#!/usr/bin/env python3
"""Task-I host context every 10 seconds; read-only, never signals processes."""
import json
import os
import socket
from pathlib import Path
import sys
import time

def background_identity(pid):
    """Only the pre-existing node9900 orcd-mini client, never arbitrary work."""
    assert socket.gethostname().split('.')[0] == 'node9900', 'background client allowed only on user-selected node9900'
    proc = Path('/proc') / str(int(pid))
    assert proc.stat().st_uid == os.getuid()
    args = (proc/'cmdline').read_bytes().decode().split('\0')
    assert Path(args[0]).name == 'efs-fuse' and args[1:3] == ['10.1.99.1:17432', '/data1/efs/orcd-mini-mnt'], args
    fields = (proc/'stat').read_text().rsplit(')',1)[1].split()
    return dict(pid=int(pid), start_ticks=int(fields[19]), cpu_ticks=int(fields[11])+int(fields[12]), command=args[:3])


def main():
    source, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
    token = json.loads(Path(sys.argv[3]).read_text()) if len(sys.argv)>3 and sys.argv[3] else None
    with output.open('x') as log:
        while True:
            started = time.time()
            foreign, background = [], []
            if token:
                try:
                    found = background_identity(token['pid'])
                    assert found['start_ticks'] == token['start_ticks'], 'background PID reused'
                    background.append(found)
                except (OSError, ValueError, AssertionError) as error:
                    foreign.append(dict(reason='background client changed',error=str(error)))
            for proc in Path('/proc').glob('[0-9]*'):
                try:
                    if proc.stat().st_uid != os.getuid():
                        continue
                    comm = (proc / 'comm').read_text().strip()
                    args = (proc / 'cmdline').read_bytes().decode(errors='replace').split('\0')
                    runner = len(args) > 1 and Path(args[1]).name == 'make_test_summary.sh'
                    if comm not in ('make', 'gmake', 'efsd', 'efs-fuse', 'gcc', 'cc1') and not runner:
                        continue
                    if token and int(proc.name)==token['pid'] and background:
                        continue
                    if any(str(source) in x for x in args) or (proc / 'cwd').resolve().is_relative_to(source):
                        continue
                    foreign.append(dict(pid=int(proc.name), comm=comm, command=args[:3]))
                except (OSError, ValueError):
                    continue
            record = dict(t=started, foreign_jobs=foreign, background_jobs=background,
                          cpu=Path('/proc/stat').read_text().splitlines()[0],
                          memory=Path('/proc/meminfo').read_text(),
                          disk=Path('/proc/diskstats').read_text(),
                          load=Path('/proc/loadavg').read_text().strip())
            log.write(json.dumps(record) + '\n')
            log.flush()
            time.sleep(max(0.1, 10 - (time.time() - started)))


if __name__ == '__main__':
    main()
