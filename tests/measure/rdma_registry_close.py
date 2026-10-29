#!/usr/bin/env python3
"""Seal closed Task AI evidence and prune only positively passed fixtures.

Run seal, inspect its plan, then prune. Failed case directories are inventoried
before and after pruning and are never deletion targets. No process is signalled,
no mount is detached, no runtime payload is copied into the retained text bundle.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess


def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1<<20),b''):h.update(b)
    return h.hexdigest()


def inventory(root):
    records=[]
    for parent,dirs,files in os.walk(root,followlinks=False):
        for name in sorted(dirs+files):
            p=Path(parent)/name;s=p.lstat()
            row=dict(path=str(p),mode=s.st_mode,uid=s.st_uid,gid=s.st_gid,links=s.st_nlink)
            if stat.S_ISREG(s.st_mode):row.update(bytes=s.st_size,sha256=digest(p))
            elif stat.S_ISLNK(s.st_mode):row['link']=os.readlink(p)
            records.append(row)
    return sorted(records,key=lambda r:r['path'])


def quiet(root):
    spec=importlib.util.spec_from_file_location('cluster',Path(__file__).with_name('rdma_registry_cluster.py'))
    helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
    found=helper.foreign_work(root)
    for p in Path('/proc').iterdir():
        if not p.name.isdigit():continue
        try:a=[x.decode(errors='replace') for x in (p/'cmdline').read_bytes().split(b'\0') if x]
        except FileNotFoundError:continue
        if a and Path(a[0]).name in ('efsd','efs-fuse') and any(str(root) in v for v in a):found.append((p.name,a))
    mounts=[x for x in Path('/proc/self/mountinfo').read_text().splitlines() if str(root) in x]
    if found or mounts:raise RuntimeError('cleanup requires no observed EFS/test work or owned mounts: '+str((found,mounts)))


def evidence_files(root):
    """Text only, skip all runtime stores (including Raft *.log files)."""
    blocked={'mdraft','meta','data','fragments','segments','store','kv','log'}
    for parent,dirs,files in os.walk(root,followlinks=False):
        dirs[:]=[d for d in dirs if d not in blocked and not re.fullmatch(r'n\d+',d)]
        for name in files:
            p=Path(parent)/name
            if (not p.is_symlink() and p.name not in ('raft.log','wal.log','kv.log') and
                    p.suffix in ('.log','.json','.tsv','.txt','.csv')):
                yield p


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('mode',choices=('seal','prune'));ap.add_argument('private',type=Path)
    ap.add_argument('retained',type=Path);a=ap.parse_args();root=a.private.resolve();out=a.retained.resolve()
    assert root.name=='efs-taskAI-20261028' and root.stat().st_uid==os.getuid()
    quiet(root)
    if a.mode=='prune':
        plan=json.loads((out/'prune-plan.json').read_text())
        assert plan['private']==str(root)
        before=json.loads((out/'failed-fixture-inventory.json').read_text())
        for r in before:
            assert inventory(Path(r['root']))==r['files'], 'failed evidence changed before cleanup'
        receipt=[]
        for target in plan['targets']:
            quiet(root)
            p=Path(target)
            assert p.is_dir() and not p.is_symlink() and p.resolve()==p and str(p).startswith(str(root)+'/')
            assert p.stat().st_uid==os.getuid()
            assert all(not (p==Path(r['root']) or p in Path(r['root']).parents) for r in before)
            done=subprocess.run([str(Path.home()/'bin/edelete'),'--force','--delete',str(p)],
                                stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            receipt.append(dict(path=str(p),rc=done.returncode,output=done.stdout))
            (out/'prune-progress.json').write_text(json.dumps(receipt,indent=2)+'\n')
            assert done.returncode==0 and not p.exists(),str(receipt[-1])
        for r in before:
            assert inventory(Path(r['root']))==r['files'],'failed evidence changed during cleanup'
        quiet(root)
        (out/'cleanup-complete.json').write_text(json.dumps(dict(deleted=len(receipt),failed_roots_unchanged=len(before),
            every_failed_regular_file_rehashed=True,no_observed_efs_test_work=True,no_owned_mounts=True,
            system_libraries_unchanged=True),indent=2)+'\n')
        return
    assert not out.exists();out.mkdir(parents=True)
    recheck=json.loads((root/'recheck-7d5e065bd/state.json').read_text())
    assert recheck['verdict']=='COMPLETE_RECORDED'
    files=set();targets=[];failed=[]
    prior=Path.home()/'efs-artifacts/taskAI-20261028/fcstor015/closed-measurements-manifest-v2.json'
    for row in json.loads(prior.read_text())['files']:
        p=Path(row['path']);assert digest(p)==row['sha256'];files.add(p)
    for src,tmp in (('native','tmp'),('gate-7d5e065bd','recheck-tmp')):
        tree=root/src
        for name in ('summary.txt','timings.tsv','cmds.txt','cmds.raw','build.log','enum.log','progress.txt'):
            files.add(tree/name)
        files.update((tree/'tmp').glob('fail-*.log'))
        skip={int(x.split('\t')[1]) for x in (tree/'summary.txt').read_text().splitlines() if x.startswith('NOTRUN\t')}
        for line in (tree/'timings.tsv').read_text().splitlines()[1:]:
            fields=line.split('\t');i,rc=int(fields[0]),int(fields[2]);case=root/tmp/f'case-{i}'
            if rc and case.exists():failed.append(case);files.update(evidence_files(case))
            elif rc==0 and i not in skip and case.is_dir():
                # The standard summary/timings are the unit-gate evidence.
                # W166 deliberately retains its result/daemon logs for review.
                if 'test_w166_authority.py' in line:files.update(evidence_files(case))
                targets.append(case)
    base=root/'base-check-complete-export'
    files.update(evidence_files(base))
    for p in (root/'base-tmp').glob('efs-b6-*'):
        failed.append(p);files.update(evidence_files(p))
    for folder in ('followup-1','followup-2','followup-3','recheck-7d5e065bd'):
        files.update(p for p in (root/folder).iterdir() if p.is_file() and p.suffix in ('.json','.log','.c'))
    for parent in ('gc-a','gc-b'):
        for gc in (root/parent).glob('gc-*'):
            files.update(evidence_files(gc));targets.append(gc)
    for folder in ('matrix-d433ab689','smoke-d433ab689'):
        assert json.loads((root/folder/'state.json').read_text())['verdict']=='PASS_SCOPED'
        for p in (root/folder).glob('*/result.json'):
            result=json.loads(p.read_text());assert result['verdict']=='PASS_SCOPED'
            assert result['daemon_exits']=={'1':0,'2':0,'3':0} and result['stop']['rc']==0
            for n in (1,2,3):
                node=p.parent/'cluster'/f'n{n}'
                assert str(node) in (p.parent/'cluster'/f'n{n}.log').read_text()
                targets.append(node)
    targets += [root/'probe',root/'deps/dnf-cache',root/'deps/dnf-state']
    # Retain exact component executables and known generated diagnostics, not
    # runtime libraries or stores. Keep native/base/final builds for OPEN K55/K53.
    files.update(root/'followup-3'/('streams-'+x) for x in ('native','probe','wake'))
    files.add(root/'collector-current-native.log');files.add(root/'k22-timeout-unmount.json')
    files.update((root/'deps').glob('*.log'))
    files.update((root/'deps/tsan-rpm').glob('*.rpm'))
    manifest=[]
    for p in sorted(files):
        assert p.is_file() and not p.is_symlink()
        h=digest(p)
        if str(p).startswith(str(root)+'/'):
            dest=out/'private'/p.relative_to(root)
        else:dest=out/'prior-raw'/p.relative_to(Path.home()/'efs-artifacts/taskAI-20261028/fcstor015')
        dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
        assert digest(dest)==h
        manifest.append(dict(source=str(p),retained=str(dest),bytes=p.stat().st_size,sha256=h))
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    failed=sorted(set(failed));snap=[dict(root=str(p),files=inventory(p)) for p in failed]
    (out/'failed-fixture-inventory.json').write_text(json.dumps(snap,indent=2)+'\n')
    (out/'prune-plan.json').write_text(json.dumps(dict(private=str(root),targets=list(map(str,sorted(set(targets)))),
        kept_failed_roots=list(map(str,failed)),kept_builds_for_open_findings=['native','base-486aa3a46','gate-7d5e065bd']),indent=2)+'\n')
    print(json.dumps(dict(retained_files=len(manifest),failed_roots=len(failed),proposed_deletions=len(set(targets)),
                         manifest_sha256=digest(out/'manifest.json'))))


if __name__=='__main__':main()
