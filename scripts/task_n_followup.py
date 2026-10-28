#!/usr/bin/env python3
"""Task N: preserve initial failures, corrected setup case, base and final gate.

Run after the original controller exits, with the corrected measurement files
already staged and an untouched devel 063154e0e export at WORK/base. Never
replaces a prior case, never overlaps the code gate with append workloads.
"""
import argparse,json,os,shutil,subprocess,sys,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
a=p.parse_args();assert os.uname().nodename.split('.')[0]=='intel1'
assert (a.out/'controller-exit').exists()
rows=[]
def owned():
    found=[]
    for proc in Path('/proc').glob('[0-9]*'):
        if proc.name==str(os.getpid()):continue
        try:
            if proc.stat().st_uid!=os.getuid():continue
            args=[os.fsdecode(s) for s in (proc/'cmdline').read_bytes().split(b'\0') if s]
            if any(s==str(a.work) or s.startswith(str(a.work)+'/') for s in args):found.append(dict(pid=proc.name,args=args))
        except OSError:pass
    assert not found,found
owned()
# Initial failure and all raw verdicts stay unchanged outside the source tree.
dest=a.out/'initial-gate';dest.mkdir(exist_ok=False)
for name in ('summary.txt','timings.tsv','progress.txt','enum.log','build.log','cmds.txt','cmds.raw'):
    file=a.work/'plain'/name
    if file.exists():shutil.copyfile(file,dest/name)
for file in (a.work/'plain/tmp').glob('fail-*.log'):shutil.copyfile(file,dest/file.name)
for tree in ('plain','fault'):
    shutil.copyfile(a.out/'driver-77556050e.py',a.work/tree/'tests/live/append_failover.py')
    shutil.copyfile(a.out/'accounting-f8ba69f36.py',a.work/tree/'tests/test_append_failover_accounting.py')
def run(label,cmd,tree,source):
    (a.out/'followup-stage').write_text(label+'\n');started=time.time()
    env=dict(os.environ,EFS_GIT_ID=source,EFS_GIT_BRANCH='agent/devel-task-n',EFS_GIT_DESCRIBE=source,EFS_BUILD_JOBS='8',EFS_TEST_TARGET='test-all',EFS_TEST_JOBS='8')
    with (a.out/(label+'.log')).open('x') as log:
        proc=subprocess.Popen(cmd,cwd=tree,env=env,stdout=log,stderr=subprocess.STDOUT)
        while proc.poll() is None:
            with (a.out/'followup-host-samples.jsonl').open('a') as samples:
                samples.write(json.dumps(dict(stage=label,wall=time.time(),cpu=Path('/proc/stat').read_text().splitlines()[0],load=Path('/proc/loadavg').read_text().strip(),diskstats=Path('/proc/diskstats').read_text()))+'\n')
            time.sleep(5)
    rows.append(dict(label=label,command=cmd,source=source,exit=proc.returncode,seconds=time.time()-started))
    (a.out/'followup-commands.json').write_text(json.dumps(rows,indent=2)+'\n')
    return proc.returncode
try:
    label='w64-lost-2-ready1';tree=a.work/'fault'
    rc=run(label,['python3','-u','tests/live/append_failover.py','--rel',str(tree),'--root',str(a.work/'runs'/label),'--port','19300','--label',label,'--records','2000','--writers','4','--peer-writers','1','--lose-reserve-every','97','--lose-reserve-max','4'],tree,'f98578e697d3')
    owned()
    shutil.copyfile(a.work/'runs'/label/'report.json',a.out/(label+'-report.json'))
    for label,tree,source in [('base-gate',a.work/'base','063154e0e815'),('final-gate',a.work/'plain','f98578e697d3')]:
        rc=run(label,['bash','scripts/make_test_summary.sh'],tree,source)
        dest=a.out/label;dest.mkdir(exist_ok=False)
        for name in ('summary.txt','timings.tsv','progress.txt','enum.log','build.log','cmds.txt','cmds.raw'):
            file=tree/name
            if file.exists():shutil.copyfile(file,dest/name)
        for file in (tree/'tmp').glob('fail-*.log'):shutil.copyfile(file,dest/file.name)
        owned()
    (a.out/'followup-result.json').write_text(json.dumps(dict(finished=time.time(),commands=rows),indent=2)+'\n')
except BaseException as exc:
    (a.out/'followup-failure.txt').write_text(repr(exc)+'\n');raise
finally:
    (a.out/'followup-stage').write_text('FINISHED\n')

sys.exit(1 if any(r["exit"] not in ((0,3) if r["label"].endswith("gate") else (0,)) for r in rows) else (3 if any(r["exit"]==3 for r in rows) else 0))
