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


def foreign_jobs(source=None, *, proc_root=Path('/proc'), current_uid=None):
    """Test/build work across all UIDs; AMD's W123 runs as a different user.

    A source match only owns a process of our UID. Unreadable metadata for
    a relevant process is an unknown competing job, never proof of quiet.
    """
    current_uid = os.getuid() if current_uid is None else current_uid
    source = Path(source).resolve() if source else None
    found = []
    for proc in proc_root.glob('[0-9]*'):
        try:
            uid = proc.stat().st_uid
            comm = (proc/'comm').read_text().strip()
            known = comm in ('make', 'gmake', 'efsd', 'efs-fuse', 'gcc', 'cc1', 'cc1plus')
            python = comm.startswith('python')
            if not known and not python and comm not in ('bash', 'sh', 'tar', 'zstd', 'rsync', 'perf'):
                continue
            try:
                args = (proc/'cmdline').read_bytes().decode(errors='replace').split('\0')
            except (PermissionError, FileNotFoundError):
                if proc.exists() and (known or python):
                    found.append(dict(pid=int(proc.name),uid=uid,comm=comm,reason='command unreadable'))
                continue
            runner = any(Path(x).name == 'make_test_summary.sh' for x in args[1:3])
            project = any('/efs/' in x or '/efs-' in x or '/efs_' in x for x in args[1:])
            traffic = python and (project or any('/tests/' in x or x.startswith('tests/') or
                                     Path(x).name in ('efsd','efs-fuse') for x in args[1:]))
            archive = comm in ('tar','zstd','rsync','perf') and project
            if not known and not runner and not traffic and not archive:
                continue
            owned = False
            if source is not None and uid == current_uid:
                owned = any(x.startswith(str(source) + '/') for x in args)
                if not owned:
                    try:
                        owned = (proc/'cwd').resolve(strict=True).is_relative_to(source)
                    except OSError:
                        pass
            if not owned:
                found.append(dict(pid=int(proc.name),uid=uid,comm=comm,command=args[:3]))
        except (FileNotFoundError, ProcessLookupError):
            continue
        except (OSError, ValueError) as error:
            found.append(dict(pid=int(proc.name),reason='process metadata unreadable',error=str(error)))
    return found


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
            jobs = foreign_jobs(source)
            if token and background:
                jobs = [job for job in jobs if job['pid'] != token['pid']]
            foreign.extend(jobs)
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
