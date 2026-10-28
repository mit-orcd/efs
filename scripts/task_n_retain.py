#!/usr/bin/env python3
"""Retain closed Task N evidence before deleting its successful private bulk.

Refuses live owners/mounts, incomplete/failed completed live cases, or any
unclassified code-gate failure. Closed W130 listing-failure stores stay on
the host for the explicitly open investigation; only other bulk is pruned.
"""
import argparse,hashlib,json,os,re,shutil,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--work',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
a=p.parse_args();work=a.work.resolve();out=a.out.resolve()
assert os.uname().nodename.split('.')[0]=='intel1' and os.getuid()!=0
assert work==Path.home()/'efs-taskN-20261027' and out==Path.home()/'efs-artifacts/taskN-20261027'
assert (out/'controller-exit').exists() and (out/'followup-exit').exists(),'controllers are not closed'
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
for stage in ('initial-gate','base-gate','final-gate'):
    text=(out/stage/'summary.txt').read_text()
    failures=[line.split('\t') for line in text.splitlines() if re.match(r'^\d+\t',line)]
    assert len(failures)==int(re.search(r' FAIL (\d+) ',text.splitlines()[0])[1]),text
    for fields in failures:
        assert fields[2]=='python3 tests/test_w130_dir_fault.py',fields
        log=(out/stage/('fail-'+fields[0]+'.log')).read_text()
        assert re.search(r'partial listing \d+ != 96',log),log[-1500:]

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
# Preserve each newly failed W130 fixture before pruning/reusing gate scratch.
# The initial fixture was already copied before the final gate reused case-475.
for stage in ('base-gate','final-gate'):
    for file in (out/stage).glob('fail-*.log'):
        text=file.read_text()
        match=re.search(r'work dir kept for forensics: (\S+)',text)
        assert match,file
        source=Path(match[1]).resolve()
        assert source.is_relative_to(work),source
        # A later run may have reused the same scratch root; only preserve
        # the currently present fixture from its own build tree.
        if not source.exists():continue
        dest=work/('open-w130-'+stage)/source.name
        assert not dest.exists(),dest
        shutil.copytree(source,dest)
        files=[]
        for original in sorted(source.rglob('*')):
            if not original.is_file():continue
            assert not original.is_symlink(),original
            copied=dest/original.relative_to(source)
            assert digest(original)==digest(copied),original
            files.append(dict(relative=str(original.relative_to(source)),sha256=digest(copied),bytes=copied.stat().st_size))
        (out/(dest.parent.name+'-files.json')).write_text(json.dumps(dict(original=str(source),preserved_copy=str(dest),files=files),indent=2)+'\n')
for kept in work.glob('open-w130-*'):
    for log in kept.rglob('*.log'):
        if len(log.relative_to(kept).parts)<=3:
            copy(log,out/'open-w130-text'/kept.name/log.relative_to(kept))
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
kept=sorted(str(path) for path in work.glob('open-w130-*'))
assert kept,'the initial open W130 failure must remain preserved'
(out/'OPEN-STORES.json').write_text(json.dumps(dict(reason='OPEN W130 partial enumeration investigation; no missing payload claim',paths=kept),indent=2)+'\n')
pruned=[]
with (out/'prune.log').open('w') as stream:
    for child in sorted(work.iterdir()):
        if str(child) in kept:continue
        assert child.is_dir() and not child.is_symlink(),child
        subprocess.run([str(Path.home()/'bin/edelete'),'--force','--delete',str(child)],stdout=stream,stderr=subprocess.STDOUT,check=True)
        pruned.append(str(child))
(out/'PRUNED.txt').write_text('Six completed live cases PASS; original lost-2 setup FAIL and initial/full-gate W130 FAIL results remain. Copied logs/results matched their closed source SHA256. Pruned only successful synthetic stores/payloads, classified empty setup stores, build/source trees and other gate scratch: '+json.dumps(pruned)+'. OPEN W130 failed stores are kept at '+json.dumps(kept)+'; their full-file manifests remain in evidence. No copied runtime store is in this evidence directory. Existing production services are untouched.\n')
# No output stream in this directory remains open after this point.
rows=[]
for file in sorted(out.rglob('*')):
    assert not file.is_symlink(),file
    if file.is_file() and file.name!='SHA256SUMS':rows.append(f'{digest(file)}  {file.relative_to(out)}\n')
(out/'SHA256SUMS').write_text(''.join(rows))
with (out.parent/'taskN-20261027-manifest-verify.log').open('w') as stream:
    subprocess.run(['sha256sum','-c','SHA256SUMS'],cwd=out,stdout=stream,stderr=subprocess.STDOUT,check=True)
print(json.dumps(dict(files=len(rows),manifest_sha256=digest(out/'SHA256SUMS'),bulk_pruned=pruned,open_stores_kept=kept)))
