#!/usr/bin/env python3
"""Verify the test interposer delays only the selected real barrier and preserves errors."""
import json,os,subprocess,sys,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
if sys.platform!='linux':
    print('slow follower sync interposer: Linux-only gate not run');sys.exit(0)
with tempfile.TemporaryDirectory(prefix='efs-slow-raft-') as directory:
    base=Path(directory);lib=base/'slow.so';marker=base/'armed'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',str(root/'tests/live/raft_slow_sync.c'),'-pthread','-ldl','-o',str(lib)],check=True)
    script=r'''
import errno,json,os,sys,time
from pathlib import Path
base=Path(sys.argv[1]);marker=base/'armed';rows=[]
for node,armed in [(2,True),(3,False),(3,True)]:
 p=base/f'n{node}/mdraft/log/raft.log';p.parent.mkdir(parents=True,exist_ok=True)
 with p.open('ab',buffering=0) as f:
  f.write(b'acknowledged test log')
  if armed:marker.write_text("3\n")
  else:marker.unlink(missing_ok=True)
  began=time.monotonic();os.fsync(f.fileno());os.fdatasync(f.fileno())
  elapsed=time.monotonic()-began;rows.append({'node':node,'armed':armed,'elapsed':elapsed})
  if node==3 and armed:assert elapsed>=0.19
marker.write_text('3\n')
with p.with_suffix('.log.tmp').open('wb') as f:
 f.write(b'not the active log');f.flush();os.fsync(f.fileno())
marker.unlink()
with p.open('rb') as f:os.fsync(f.fileno())
bad=os.open(p,os.O_RDONLY);os.close(bad)
try:os.fsync(bad)
except OSError as error:assert error.errno==errno.EBADF
else:raise AssertionError('bad fd was accepted')
assert p.read_bytes()==b'acknowledged test log'*2
print(json.dumps(rows))
'''
    result=subprocess.run([sys.executable,'-c',script,str(base)],env=os.environ|{'LD_PRELOAD':str(lib),'EFS_TEST_RAFT_SLOW_FILE':str(marker)},capture_output=True,text=True,timeout=10)
    if result.returncode:print(result.stdout,result.stderr,file=sys.stderr)
    result.check_returncode()
    assert result.stderr.count('slow-raft-sync armed')==1
    assert 'slow-raft-sync healed node=3 delayed_calls=2' in result.stderr
    print(result.stdout.strip());print('slow follower: real fsync/fdatasync bytes, selected path/marker, real EBADF and healing PASS')
