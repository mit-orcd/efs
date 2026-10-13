#!/usr/bin/env python3
"""Normal timeout cleanup uses the existing bounded state-aware group oracle."""
import importlib.util,os
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=Path(os.environ.get('EFS_POSIX_SOURCE',root/'tests/posix/posix_suite.py'))
spec=importlib.util.spec_from_file_location('posix_zombie_test',p);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Proc:
 pid=424242
 def poll(self):return 0
m._pgid_alive_hook=lambda _pg:True
signals=[];m._signal_group_hook=lambda pg,sig:signals.append((pg,sig))
m._reap_hook=lambda _pid:None
for snapshot,expected in [(b'424243 424242 Z\n',True),(b'424243 424242 D\n',False),(b'424243 424242 S\n',False),(None,False),(b'424243 424242 Z\n424244 17 S\n',True)]:
 m._ps_hook=lambda _timeout,snapshot=snapshot:snapshot
 signals.clear();assert m._kill_group(Proc(),grace=.01)==expected,(snapshot,expected)
 assert signals and all(pg==Proc.pid for pg,_sig in signals)
 assert signals[0][1]==m.signal.SIGTERM and signals[-1][1]==m.signal.SIGKILL
# A missing group needs no signal or process-table read.
m._pgid_alive_hook=lambda _pg:False
m._ps_hook=lambda _timeout:(_ for _ in ()).throw(AssertionError('unexpected ps'))
signals.clear();assert m._kill_group(Proc(),grace=.01) and not signals
print('POSIX timeout: zombie-only completion, live/D/unknown refusal, bounded original grace, scoped signals PASS')
