#!/usr/bin/env python3
"""Final current-test-overlay native suite on a separate NUC source copy."""
import hashlib,json,os,signal,subprocess,time,traceback
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261011');out=base/'final-native';out.mkdir(exist_ok=False)
loaded=base/'loaded-append-v2';deadline=1791730579
try:
 while not (loaded/'restore-exit.txt').exists():
  if (loaded/'failure.txt').exists() and not (loaded/'mnt-stop-exit.txt').exists():break
  assert time.time()<deadline-10*60,'no time left for a final native suite; prior corrected full native remains accepted'
  time.sleep(.5)
 assert time.time()<deadline-10*60,'retain prior corrected full native acceptance; do not start an overlong rerun'
 overlay=base/'final-native-overlay';revision=(overlay/'revision.txt').read_text().strip()
 src=base/'final-native-src';assert not src.exists()
 subprocess.run(['cp','-a','--reflink=auto',str(base/'candidate'),str(src)],check=True)
 subprocess.run(['rsync','-a',str(overlay)+'/',str(src)+'/' ],check=True)
 paths=[p for folder in ('src','include','deps','tests','docs') for p in (src/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.py','.md','.html')]
 paths.append(src/'Makefile')
 (out/'source-sha256.json').write_text(json.dumps({str(p.relative_to(src)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
 command=['make','-j4','test','EFS_GIT_ID='+revision+'-nuc','EFS_GIT_DESCRIBE='+revision+'-nuc','EFS_GIT_BRANCH=devel-nuc']
 (out/'scope.json').write_text(json.dumps(dict(revision=revision,engine_core='108afa3e',command=command,scope='Separate source copy, current tracked test/docs/Makefile overlay. No installed source or cluster store changes. Live acceptance remains on the identical core built as108afa3e-nuc.'),indent=2))
 with (out/'clean.log').open('w') as log:subprocess.run(['make','clean'],cwd=src,stdout=log,stderr=subprocess.STDOUT,check=True)
 with (out/'native.log').open('w') as log:
  proc=subprocess.Popen(command,cwd=src,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
  try:rc=proc.wait(timeout=420)
  except subprocess.TimeoutExpired:
   assert os.getpgid(proc.pid)==proc.pid
   os.killpg(proc.pid,signal.SIGTERM);proc.wait(timeout=30)
   raise RuntimeError('final native timed out; prior full native acceptance retained, current rerun not accepted')
 (out/'native-exit.txt').write_text(str(rc)+'\n');assert rc==0
 for name in ('test_fixture_interrupt.py','test_nuc_block_analysis.py','test_nuc_controller_timeout.py'):
  with (out/(name+'.log')).open('w') as log:
   rc=subprocess.run(['python3','tests/'+name],cwd=src,stdout=log,stderr=subprocess.STDOUT,timeout=30).returncode
  (out/(name+'-exit.txt')).write_text(str(rc)+'\n');assert rc==0
 (out/'run-exit.txt').write_text('0\n')
 print('final current-test-overlay native suite PASS',flush=True)
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());raise
