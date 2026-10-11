#!/usr/bin/env python3
"""Real gate runner cancels an owned fixture even with nohup signal inheritance."""
from pathlib import Path
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
runner=Path(__file__).resolve().parent/'live/gate_rounds.py'
with tempfile.TemporaryDirectory(prefix='efs-gate-cancel-') as tmp:
 base=Path(tmp);repo=base/'repo';live=repo/'tests/live';live.mkdir(parents=True)
 shutil.copy2(runner,live/'gate_rounds.py')
 for name in ('efsd','efs-fuse','efs-mgmt'):(repo/name).write_bytes(b'fixture binary identity')
 state=base/'state';state.mkdir();out=base/'out'
 (live/'gc_reclamation.py').write_text('''import json,os,signal,subprocess,sys,time
from pathlib import Path
state=Path(os.environ['EFS_GATE_CANCEL_STATE'])
child=None
try:
 child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'])
 (state/'ready.json').write_text(json.dumps({'fixture_pid':os.getpid(),'daemon_pid':child.pid,'ignored':signal.getsignal(signal.SIGINT)==signal.SIG_IGN}))
 time.sleep(60)
finally:
 if child is not None:
  child.terminate();child.wait(timeout=3)
 (state/'cleaned').write_text('owned child collected')
''')
 env=dict(os.environ,EFS_GATE_CANCEL_STATE=str(state))
 with (base/'runner.log').open('w') as log:
  previous=signal.signal(signal.SIGINT,signal.SIG_IGN)
  try:p=subprocess.Popen([sys.executable,live/'gate_rounds.py','--output',out,'--start','1','--end','2','--keep-going','--port','28510'],env=env,stdout=log,stderr=subprocess.STDOUT)
  finally:signal.signal(signal.SIGINT,previous)
  ready=None
  try:
   end=time.monotonic()+10
   while time.monotonic()<end:
    if (state/'ready.json').exists():ready=json.loads((state/'ready.json').read_text());break
    assert p.poll() is None,(base/'runner.log').read_text()
    time.sleep(.02)
   assert ready,'fixture did not start'
   assert not ready['ignored'],'fixture inherited ignored SIGINT'
   p.send_signal(signal.SIGINT)
   assert p.wait(timeout=10)==130,(base/'runner.log').read_text()
   assert (state/'cleaned').read_text()=='owned child collected'
   record=json.loads((out/'round01.json').read_text())
   assert record['exit_code']==130 and 'cleanup_incomplete_pid' not in record,record
   assert record['command'][record['command'].index('--port')+1]=='28510'
   assert not (out/'round02.json').exists(),'cancelled matrix launched another round'
  finally:
   if p.poll() is None:p.terminate();p.wait(timeout=3)
   # Failure cleanup applies only to this synthetic fixture's recorded children.
   if ready and not (state/'cleaned').exists():
    for key in ('fixture_pid','daemon_pid'):
     try:os.kill(ready[key],signal.SIGTERM)
     except ProcessLookupError:pass
print('gate cancellation: nohup SIGINT restored, owned fixture finally runs, child reaped, verdict130, no later round PASS')
