#!/usr/bin/env python3
"""Serial Task S qualification on owned intel1 source; stop on first failure.
Usage: task_s_qualify.py FULL_COMMIT ARTIFACT_ROOT
"""
import json,os,socket,subprocess,sys,time,hashlib,shutil
from pathlib import Path
source=Path(__file__).resolve().parents[1]
commit,out=sys.argv[1:];art=Path(out).resolve()
assert socket.gethostname().split('.')[0]=='intel1'
assert str(source).startswith(str(Path.home()/'efs-taskS')+'/')
assert len(commit)==40 and all(c in '0123456789abcdef' for c in commit)
assert art.parent==Path.home()/'efs-artifacts/taskS-20261028'
art.mkdir(parents=True,exist_ok=False)
os.chdir(source)
os.environ.update(EFS_GIT_ID=commit,EFS_GIT_DESCRIBE=commit,EFS_GIT_BRANCH='agent/devel-task-s',
 EFS_FUSE_PREFIX='/usr',EFS_BUILD_JOBS='8',EFS_TEST_JOBS='8',EFS_TEST_TARGET='test-all')
rows=[];rc=1
(art/'source.txt').write_text(commit+'\n'+str(source)+'\n')
files=[p for p in sorted(source.rglob('*')) if p.is_file()]
(art/'source-SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(source))+'\n' for p in files))
def quiet():
 ps=subprocess.check_output(['ps','-u',str(os.getuid()),'-o','pid=,comm=,args='],text=True)
 bad=[l for l in ps.splitlines() if any('/'+name+' ' in l for name in ('efsd','efs-fuse'))]
 assert not bad,'private EFS still active: '+str(bad)
def run(label,cmd,timeout=900):
 (art/'stage').write_text(label+'\n');start=time.monotonic()
 tmp=source.parent/'tmp'/label;tmp.mkdir(parents=True,exist_ok=True)
 env=dict(os.environ,TMPDIR=str(tmp),EFS_K33_KEEP='1',W130_EVIDENCE_DIR=str(art/'w130-logs'/label))
 if label=='gate':env.pop('EFS_K33_KEEP',None)
 with (art/(label+'.log')).open('x') as f:
  try:p=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=timeout);status=p.returncode
  except subprocess.TimeoutExpired:status=124
 rows.append(dict(label=label,argv=cmd,seconds=round(time.monotonic()-start,3),rc=status))
 (art/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
 return status
try:
 quiet()
 for label,cmd in [('build',['make','-j8','test-bins']),('meta',['./tests/test_meta_apply']),
  ('cursor',['python3','tests/test_readdir_source_cursor.py']),('boundary',['python3','tests/test_readdir_reply_boundary.py']),
  ('inventory',['python3','tests/test_env_inventory.py']),('recipe',['python3','tests/test_make_test_recipe.py'])]:
  assert run(label,cmd)==0,label+' failed'
 with (art/'version.txt').open('w') as f:subprocess.run(['./efsd','--version'],stdout=f,stderr=subprocess.STDOUT,check=True)
 # Verify the pause is absent from the normal product and present only in the fault daemon.
 normal=subprocess.check_output(['strings','efsd'],text=True);fault=subprocess.check_output(['strings','tests/faults/efsd'],text=True)
 assert 'fault k48-readdir:' not in normal and 'fault k48-readdir:' in fault
 (art/'fault-build-scope.txt').write_text('normal product: pause absent; fault daemon: pause present\n')
 for i in range(1,4):
  quiet();assert run(f'k48-{i}',['python3','tests/test_k48_readdir.py'])==0,'K48 deterministic failed'
 for i in range(1,11):
  for label,script in [('w130','tests/test_w130_dir_fault.py'),('payload','tests/test_k33_payload.py')]:
   quiet();assert run(f'{label}-{i}',['python3',script])==0,label+' failed'
 quiet()
 status=run('gate',['bash','scripts/make_test_summary.sh'],3600)
 dest=art/'code-gate';dest.mkdir()
 for name in ('summary.txt','timings.tsv','progress.txt','build.log','enum.log','cmds.txt','cmds.raw'):
  if (source/name).exists():shutil.copy2(source/name,dest/name)
 for p in (source/'tmp').glob('fail-*.log'):shutil.copy2(p,dest/p.name)
 quiet();assert status in (0,3),'full gate failed'
 rc=status;(art/'stage').write_text('FINISHED\n')
except Exception as e:
 (art/'error.txt').write_text(repr(e)+'\n');(art/'stage').write_text('STOPPED\n')
finally:(art/'controller.exit').write_text(str(rc)+'\n')
sys.exit(rc)
