#!/usr/bin/env python3
"""Orderly AMD-only v5->v6 cache transition; no Raft/data deletion.
Run as efs on amd_efs with exclusive cluster access and prebuilt baseline/candidate.
Raw outputs belong outside Git. On failure attempt orderly candidate restoration.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('baseline', type=Path)
p.add_argument('candidate', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--private-budget', action='store_true', help='64/80 MiB generation/aggregate control, then restore defaults')
a = p.parse_args()
base = Path('/data1/efs')
assert os.getuid() != 0 and os.environ.get('USER') == 'efs'
assert base.is_dir() and a.baseline.resolve() != a.candidate.resolve()
a.output.mkdir(parents=True, exist_ok=False)
env = dict(os.environ, EFS_CLUSTER='amd', EFS_COMMANDS='1', EFS_COMMANDS_SERVER='1',
           EFS_EXPIRY='0', EFS_REPAIR_EXPERIMENT='0',
           EFS_TRANSPORT='tcp', EFS_DIRECT_IO='1')
if a.private_budget: env.update(EFS_QUERY_DISK_MAX_MIB='64',EFS_QUERY_DISK_TOTAL_MIB='80')

def run(label, command):
    with (a.output/(label+'.log')).open('wb') as f:
        subprocess.run(command, env=env, stdout=f, stderr=subprocess.STDOUT,
                       check=True, timeout=300)

def stop(label): run(label, [str(base/'devops/stop.sh')])
def start(label): run(label, [str(base/'devops/start.sh')])
def install(source):
    for name in ('efsd', 'efs-fuse', 'efs-mgmt'):
        assert (source/name).is_file()
        shutil.copy2(source/name, base/'src'/name)

def check(label):
    run(label, ['python3', str(a.candidate/'tests/live/efs_query_small_lifecycle.py'),
                str(base/'mnt'), str(base/'mnt2'), 'check'])

def inventory(version):
    rows=[]
    for manifest in sorted(base.glob('n*/mdraft/query-cache/current/manifest')):
        h=struct.unpack('12Q', manifest.read_bytes()[:96])
        assert h[1] == version, (str(manifest), h)
        generation=manifest.resolve().parent
        rows.append(dict(path=str(manifest), generation=str(generation), version=h[1], row_bytes=h[3], rows=h[4],
                         bytes=sum(f.stat().st_size for f in generation.iterdir() if f.is_file()),
                         manifest_sha256=hashlib.sha256(manifest.read_bytes()).hexdigest()))
    assert rows, 'no actual published cache'
    return rows

def wait_inventory(version):
    # A successful routed query proves one publisher, not every publisher.
    # Require both AMD query-capable nodes before stopping the baseline.
    import time
    deadline=time.monotonic()+30
    while True:
        try:
            rows=inventory(version)
            if len(rows)==2:return rows
        except (AssertionError,FileNotFoundError,struct.error): pass
        assert time.monotonic()<deadline, ('publisher readiness',version)
        time.sleep(.2)

result={}
try:
    stop('initial-stop');install(a.baseline);start('v5-start');check('v5-payload')
    wait_inventory(5)
    stop('v5-stop');result['old']=inventory(5)
    log_offsets={p:p.stat().st_size for p in (base/'logs').glob('efsd-n*.log')}
    install(a.candidate);start('v6-start');check('v6-payload')
    result['new']=wait_inventory(6)
    # No intervening daemon restart may hide a leaked old-format directory.
    assert all(not Path(row['generation']).exists() for row in result['old']), 'legacy generation leaked'
    marker=base/'mnt/package2-transition-refresh'
    marker.mkdir()
    with (marker/'marker').open('wb') as f:
        f.write(b'next-refresh');f.flush();os.fsync(f.fileno())
    import time
    deadline=time.monotonic()+25
    while True:
        cmd=['python3',str(marker/'.efs/find'),'marker','--json']
        fresh=subprocess.run(cmd,env=env,capture_output=True,timeout=30)
        if not fresh.returncode:
            data=json.loads(fresh.stdout)
            if data['complete'] and [e['path_hex'] for e in data['entries']]==[b'marker'.hex()]:break
        assert time.monotonic()<deadline, ('second refresh failed', fresh.stderr)
        time.sleep(.2)
    assert (base/'mnt2'/marker.name/'marker').read_bytes()==b'next-refresh'
    result['second_refresh']=inventory(6)
    result['private_budget']=a.private_budget
    (marker/'marker').unlink();marker.rmdir()

    rejected=[]
    for log,offset in log_offsets.items():
        with log.open('rb') as f: f.seek(offset);tail=f.read()
        if b'query-restore: rc=-7 backend=disk' in tail: rejected.append(str(log))
    assert rejected, 'no observed legacy-format rejection/rebuild'
    result['legacy_rejected_logs']=rejected
    run('policy', [str(base/'src/efs-mgmt'), 'commands-policy', '127.0.0.1:17432', 'get'])
    assert 'mask=3' in (a.output/'policy.log').read_text()
    if a.private_budget:
        stop('private-budget-stop')
        env.pop('EFS_QUERY_DISK_MAX_MIB');env.pop('EFS_QUERY_DISK_TOTAL_MIB')
        start('default-budget-start');check('default-budget-payload')
    result['verdict']='PASS'
except BaseException:
    result['verdict']='FAIL'
    env.pop('EFS_QUERY_DISK_MAX_MIB',None);env.pop('EFS_QUERY_DISK_TOTAL_MIB',None)
    try: stop('failure-stop');install(a.candidate);start('failure-candidate-start')
    except BaseException: result['restore_failed']=True
    raise
finally:
    (a.output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result), flush=True)
