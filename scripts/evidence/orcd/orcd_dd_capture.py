#!/usr/bin/env python3
"""Same-user Linux sampling of one explicitly identified ORCD-test process.

No ptrace, privileged settings, product switches, or production signals.
Only the sampler's own process groups are stopped. Raw output is external.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('kind', choices=('efsd', 'efs-fuse'))
    ap.add_argument('out', type=Path)
    ap.add_argument('stop', type=Path)
    ap.add_argument('--seconds', type=int, default=100)
    a = ap.parse_args()
    candidates = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit():
            continue
        try:
            if p.stat().st_uid != os.getuid() or (p/'comm').read_text().strip() != a.kind:
                continue
            args = (p/'cmdline').read_bytes().split(bytes([0]))
            target = (b'/tmp/efs-mount' in args if a.kind == 'efs-fuse' else
                      any(b'/efs-orcd-test-erbmi1' in x for x in args))
            if target:
                candidates.append((p, [x.decode() for x in args if x]))
        except (FileNotFoundError, ProcessLookupError):
            pass
    assert len(candidates) == 1, candidates
    p, args = candidates[0]
    a.out.mkdir(parents=True, exist_ok=False)
    identity = dict(pid=int(p.name), args=args, start=(p/'stat').read_text(),
                    time_ns=time.time_ns(), monotonic_ns=time.monotonic_ns(),
                    executable=str((p/'exe').resolve()),
                    executable_sha256=hashlib.sha256((p/'exe').read_bytes()).hexdigest())
    (a.out/'identity.json').write_text(json.dumps(identity, indent=2)+'\n')
    children = []
    handles = []

    def launch(name, cmd):
        f = (a.out/(name+'.log')).open('wb'); handles.append(f)
        proc = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT,
                                start_new_session=True)
        children.append((name, proc, cmd))

    launch('cpu', ['perf', 'record', '-e', 'cycles:u', '-F', '99',
                  '--call-graph', 'dwarf,8192', '-p', p.name,
                  '-o', str(a.out/'cpu.data'), '--', 'sleep', str(a.seconds)])
    launch('switch', ['perf', 'record', '--switch-events', '-e', 'context-switches',
                     '-c', '100', '--call-graph', 'dwarf,4096', '-p', p.name,
                     '-o', str(a.out/'switch.data'), '--', 'sleep', str(a.seconds)])
    launch('pidstat', ['pidstat', '-t', '-u', '-r', '-d', '-w', '-p', p.name,
                      '1', str(a.seconds)])
    launch('iostat', ['iostat', '-x', '-m', '1', str(a.seconds)])
    (a.out/'commands.json').write_text(json.dumps(
        {name: cmd for name, _, cmd in children}, indent=2)+'\n')
    (a.out/'ready').write_text(str(time.time())+'\n')
    deadline = time.monotonic()+a.seconds
    with (a.out/'threads.jsonl').open('w') as out:
        while time.monotonic() < deadline and not a.stop.exists():
            rows = []
            for t in (p/'task').iterdir():
                row = dict(tid=int(t.name))
                for name in ('comm', 'stat', 'schedstat', 'wchan'):
                    try:
                        row[name] = (t/name).read_text().strip()
                    except (PermissionError, FileNotFoundError) as e:
                        row[name] = type(e).__name__
                rows.append(row)
            out.write(json.dumps(dict(time_ns=time.time_ns(), threads=rows))+'\n')
            out.flush(); time.sleep(.5)
    statuses = {}
    for name, proc, _ in children:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGINT)
        try:
            statuses[name] = proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            statuses[name] = proc.wait()
    for f in handles:
        f.close()
    (a.out/'capture-status.json').write_text(json.dumps(statuses, indent=2)+'\n')
    for kind in ('cpu', 'switch'):
        for mode, options in [('flat', ['--no-children', '--call-graph', 'none']),
                              ('callers', ['--children'])]:
            with (a.out/(kind+'-'+mode+'.txt')).open('w') as out:
                try:
                    subprocess.run(['perf', 'report', '--stdio', '--percent-limit', '1',
                                    '-i', str(a.out/(kind+'.data')), *options],
                                   stdout=out, stderr=subprocess.STDOUT, timeout=60)
                except subprocess.TimeoutExpired:
                    out.write('\nREPORT TIMEOUT; raw capture retained, no verdict inferred.\n')
    print(json.dumps(dict(out=str(a.out), status=statuses)), flush=True)


if __name__ == '__main__':
    main()
