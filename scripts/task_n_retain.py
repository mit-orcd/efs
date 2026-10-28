#!/usr/bin/env python3
"""Retain closed Task N evidence before deleting its successful private bulk.

Refuses a live owner/mount, an incomplete/failed live gate or a failing code
gate. Raw logs/results remain outside Git; no runtime store is copied.
"""
import argparse,hashlib,json,os,shutil,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--work',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
a=p.parse_args();work=a.work.resolve();out=a.out.resolve()
assert os.uname().nodename.split('.')[0]=='intel1' and os.getuid()!=0
assert work==Path.home()/'efs-taskN-20261027' and out==Path.home()/'efs-artifacts/taskN-20261027'
assert (out/'controller-exit').exists(),'controller is not closed'
assert not out.is_relative_to(work)
left=[]
for proc in Path('/proc').glob('[0-9]*'):
    if proc.name==str(os.getpid()):continue
    try:
        if proc.stat().st_uid!=os.getuid():continue
        args=[os.fsdecode(s) for s in (proc/'cmdline').read_bytes().split(b'\0') if s]
        if any(s==str(work) or s.startswith(str(work)+'/') for s in args):left.append(dict(pid=proc.name,args=args))
    except OSError:pass
mounts=[s for s in Path('/proc/self/mountinfo').read_text().splitlines() if str(work)+'/' in s]
assert not left and not mounts,(left,mounts)
labels=[f'{kind}-{n}' for kind in ('w52','w64-stepdown','w64-lost') for n in (1,2)]
success_labels=[label if label!='w64-lost-2' else 'w64-lost-2-ready1' for label in labels]
for label in success_labels:
    report=json.loads((work/'runs'/label/'report.json').read_text())
    assert report['verdict']=='PASS',(label,report.get('exception'),report.get('checks'))
summary=(work/'plain/summary.txt').read_text()
assert ' FAIL 0 ' in summary.splitlines()[0],summary[:1000]
failed_setup=json.loads((work/'runs/w64-lost-2/report.json').read_text())
assert failed_setup['verdict']=='FAIL' and not failed_setup['checks'] and 'cluster never formed' in failed_setup.get('exception','')
assert not list((work/'runs/w64-lost-2').glob('writer-*-progress.json'))
checks=[]
def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda:source.read(1<<20),b''):h.update(block)
    return h.hexdigest()
def copy(source,dest):
    assert not source.is_symlink(),source
    dest.parent.mkdir(parents=True,exist_ok=True)
    before=digest(source);shutil.copyfile(source,dest);after=digest(dest)
    assert before==after,(source,dest)
    checks.append(dict(source=str(source),destination=str(dest.relative_to(out)),sha256=before))
for label in labels+['w64-lost-2-ready1']:
    root=work/'runs'/label
    for file in root.glob('*.json'):copy(file,out/'cases'/label/file.name)
    for file in (root/'logs').glob('*.log'):copy(file,out/'cases'/label/'logs'/file.name)
for name in ('summary.txt','timings.tsv','progress.txt','enum.log','build.log','cmds.txt','cmds.raw'):
    file=work/'plain'/name
    if file.exists():copy(file,out/'gate'/name)
for file in (work/'plain/tmp').glob('fail-*.log'):copy(file,out/'gate'/file.name)
for build in ('plain','fault'):
    file=work/build/'scripts/task_n_append_gates.py'
    copy(file,out/(build+'-controller-source.py'))
    result=subprocess.check_output([str(work/build/'efsd'),'--version'],text=True)
    (out/(build+'-final-identity.txt')).write_text(result)
(out/'copy-check.json').write_text(json.dumps(checks,indent=2)+'\n')
(out/'final-state.json').write_text(json.dumps(dict(wall=time.time(),private_processes=left,
    private_mounts=mounts,successful_live_cases=success_labels,
    classified_setup_failure='w64-lost-2',gate=summary.splitlines()[0]),indent=2)+'\n')
(out/'PRUNED.txt').write_text('After verifying all six completed live cases PASS, and preserving the original w64-lost-2 setup FAIL (raw NOT_PRIMARY hint was not followed; no writer or payload was created), the full gate has zero FAIL, no private owner/mount remains, and every copied log/result SHA256 matches its closed source: remove '+str(work)+'. This prunes all successful synthetic stores/payloads, source/build trees and gate scratch. No copied runtime WAL/store is kept in evidence. Exact store replay is forfeited; committed source, commands, full record-oracle reports, latency files and text logs reproduce the results. Existing production services/stores are untouched.\n')
with (out/'prune.log').open('w') as stream:
    subprocess.run([str(Path.home()/'bin/edelete'),'--force','--delete',str(work)],stdout=stream,stderr=subprocess.STDOUT,check=True)
# No output stream in this directory remains open after this point.
rows=[]
for file in sorted(out.rglob('*')):
    assert not file.is_symlink(),file
    if file.is_file() and file.name!='SHA256SUMS':rows.append(f'{digest(file)}  {file.relative_to(out)}\n')
(out/'SHA256SUMS').write_text(''.join(rows))
with (out.parent/'taskN-20261027-manifest-verify.log').open('w') as stream:
    subprocess.run(['sha256sum','-c','SHA256SUMS'],cwd=out,stdout=stream,stderr=subprocess.STDOUT,check=True)
print(json.dumps(dict(files=len(rows),manifest_sha256=digest(out/'SHA256SUMS'),bulk_pruned=str(work))))
