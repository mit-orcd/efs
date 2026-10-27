#!/usr/bin/env python3
"""Task N intel1: two sequential runs of each live append gate, then full gate.

No product switches/deadline changes. Normal/fault source trees must contain
one identical committed source export. Output and fixture paths must be new.
The controller never retries a failed case; all evidence stays on the host.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--work',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--source',required=True)
a=p.parse_args()
assert os.uname().nodename.split('.')[0]=='intel1'
assert os.getuid()!=0
assert not (a.out/'controller-result.json').exists()
a.out.mkdir(parents=True,exist_ok=True)
plain=a.work/'plain';fault=a.work/'fault'
env=dict(os.environ,EFS_GIT_ID=a.source,EFS_GIT_BRANCH='agent/devel-task-n',EFS_GIT_DESCRIBE=a.source)
rows=[]
def run(label,cmd,cwd,extra=None):
    log=a.out/(label+'.log')
    assert not log.exists(),log
    (a.out/'stage').write_text(label+'\n')
    begin=time.time()
    with log.open('w') as stream:
        process=subprocess.Popen(cmd,cwd=cwd,env=dict(env,**(extra or {})),stdout=stream,stderr=subprocess.STDOUT)
        while process.poll() is None:
            # CPU iowait and independent service load remain attributable to
            # the same interval as every append progress/log timestamp.
            with (a.out/'host-samples.jsonl').open('a') as sample:
                stat=Path('/proc/stat').read_text().splitlines()[0]
                sample.write(json.dumps(dict(wall=time.time(),stage=label,cpu=stat,
                    load=Path('/proc/loadavg').read_text().strip(),
                    diskstats=Path('/proc/diskstats').read_text()))+'\n')
            time.sleep(5)
        rc=process.wait()
    rows.append(dict(label=label,command=cmd,cwd=str(cwd),exit=rc,seconds=time.time()-begin))
    (a.out/'commands.json').write_text(json.dumps(rows,indent=2)+'\n')
    return rc

try:
    for name,tree,flags in [('plain',plain,[]),('fault',fault,['EFS_FAULTS=1'])]:
        rc=run('build-'+name,['make','-j8',*flags,'efsd','efs-fuse','efs-mgmt'],tree)
        assert rc==0,('build failed',name)
        result=subprocess.check_output([str(tree/'efsd'),'--version'],text=True)
        (a.out/(name+'-identity.txt')).write_text(result)
        assert a.source in result,result
        with (a.out/(name+'-binaries.sha256')).open('w') as out:
            subprocess.run(['sha256sum','efsd','efs-fuse','efs-mgmt'],cwd=tree,stdout=out,check=True)
    for kind,tree,flags in [
        ('w52',plain,['--records','20000','--writers','4']),
        ('w64-stepdown',plain,['--records','8000','--writers','4','--failover','step-down','--interval','4','--first-event','3']),
        ('w64-lost',fault,['--records','2000','--writers','4','--peer-writers','1','--lose-reserve-every','97','--lose-reserve-max','4'])]:
        for attempt in (1,2):
            label=f'{kind}-{attempt}'
            fixture=a.work/'runs'/label
            rc=run(label,['python3','-u','tests/live/append_failover.py','--rel',str(tree),
                '--root',str(fixture),'--port','19300','--label',label,*flags],tree)
            # Preserve the failure and proceed only when its owned processes
            # really stopped. A failed clean stop never hands this host over.
            for proc in Path('/proc').glob('[0-9]*'):
                try:args=(proc/'cmdline').read_bytes().split(b'\0')
                except OSError:continue
                if any(str(fixture).encode() in arg for arg in args):
                    raise RuntimeError(f'case {label} retained a live owner {proc}')
            report=fixture/'report.json'
            if report.exists():
                (a.out/(label+'-report.json')).write_bytes(report.read_bytes())
    (a.work/'gate-tmp').mkdir(exist_ok=True)
    rc=run('gate',['bash','scripts/make_test_summary.sh'],plain,
        dict(EFS_TEST_TARGET='test-all',EFS_TEST_JOBS='8',TMPDIR=str(a.work/'gate-tmp')))
    for name in ('summary.txt','timings.tsv','progress.txt','enum.log','build.log','cmds.txt','cmds.raw'):
        src=plain/name
        if src.exists():(a.out/('gate-'+name)).write_bytes(src.read_bytes())
    for src in (a.work/'gate-tmp').glob('fail-*.log'):
        (a.out/src.name).write_bytes(src.read_bytes())
    (a.out/'controller-result.json').write_text(json.dumps(dict(finished=time.time(),
        verdict='PASS' if all(row['exit']==0 for row in rows) else 'FAIL',commands=rows),indent=2)+'\n')
except BaseException as exc:
    (a.out/'controller-failure.txt').write_text(repr(exc)+'\n')
    raise
finally:
    (a.out/'stage').write_text('FINISHED\n')
