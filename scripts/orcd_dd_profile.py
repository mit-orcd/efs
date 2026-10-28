#!/usr/bin/env python3
"""Coordinate one profiled dd on the explicitly scoped ORCD-test cluster.

Run from the local checkout with its normal SSH agent. No BatchMode, deploy,
reformat, kernel setting or application signal. Capture data stays on ORCD.
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import shlex
import subprocess
import time

LOGIN = 'erbmi1@orcd-login.mit.edu'
HOSTS = ['fcstor003', 'fcstor004', 'fcstor005', 'fcstor006', 'fcstor007']


def run(host, command, timeout=150):
    inner = shlex.join(['ssh', '-A', '-o', 'ConnectTimeout=15',
                        'erbmi1@'+host, command])
    r = subprocess.run(['ssh', '-A', '-o', 'ConnectTimeout=15', LOGIN, inner],
                       capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError((host, r.returncode, r.stdout, r.stderr))
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--label', required=True)
    ap.add_argument('--seconds', type=int, default=120)
    a = ap.parse_args()
    assert a.label.replace('-', '').replace('_', '').isalnum()
    root = '/home/erbmi1/orcd/scratch/efs/orcd-test/dd-profile-20261028/'+a.label
    capture = Path(__file__).with_name('orcd_dd_capture.py').read_text()
    command = 'test ! -e '+shlex.quote(root)+' && mkdir -p '+shlex.quote(root)+" && cat > "+shlex.quote(root+'/capture.py')
    subprocess.run(['ssh', '-A', '-o', 'ConnectTimeout=15', LOGIN, command],
                   input=capture, text=True, check=True)
    stop = root+'/dd.done'

    def sample(h):
        kind = 'efs-fuse' if h == 'fcstor007' else 'efsd'
        return run(h, shlex.join(['python3', root+'/capture.py', kind,
                                 root+'/'+h, stop, '--seconds', str(a.seconds)]),
                   timeout=a.seconds+90)

    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
        jobs = {h: pool.submit(sample, h) for h in HOSTS}
        try:
            deadline = time.monotonic()+40
            while True:
                ready = run('fcstor007', "python3 - <<'READY_EOF'\nimport pathlib\np=pathlib.Path("+repr(root)+")\nprint(sum((p/h/'ready').exists() for h in "+repr(HOSTS)+"))\nREADY_EOF\n")
                if int(ready.strip()) == 5:
                    break
                assert time.monotonic() < deadline, 'capture readiness deadline'
                time.sleep(1)
            time.sleep(2)
            payload = '/tmp/efs-mount/dd-profile-'+a.label+'.bin'
            code = '''import pathlib,subprocess,time,json
p=pathlib.Path(PAYLOAD);assert not p.exists(),p
cmd=['dd','if=/dev/zero','of='+str(p),'bs=1M','count=8192','conv=fdatasync','status=progress']
start=time.time_ns();t=time.monotonic()
with open(OUT+'/dd.log','wb') as log:r=subprocess.run(cmd,stdout=log,stderr=log)
wall=time.monotonic()-t
d=dict(command=cmd,rc=r.returncode,start_ns=start,end_ns=time.time_ns(),wall_s=wall,bytes=p.stat().st_size,MiB_s=p.stat().st_size/(1<<20)/wall)
pathlib.Path(OUT+'/dd-result.json').write_text(json.dumps(d,indent=2)+'\\n');print(json.dumps(d))
assert r.returncode==0 and d['bytes']==8589934592,d
'''.replace('PAYLOAD', repr(payload)).replace('OUT', repr(root))
            print(run('fcstor007', "python3 - <<'DD_EOF'\n"+code+'\nDD_EOF\n',
                      timeout=a.seconds+60), flush=True)
        finally:
            run('fcstor007', 'touch '+shlex.quote(stop))
        for h, job in jobs.items():
            try:
                print(h, job.result(), flush=True)
            except Exception as e:
                print(h, 'CAPTURE FAILED', repr(e), flush=True)
                raise
    print(json.dumps(dict(evidence=root)), flush=True)


if __name__ == '__main__':
    main()
