#!/usr/bin/env python3
"""A different account's EFS tests cannot be mistaken for a quiet host."""
import os,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from task_i_host_sample import foreign_jobs
with tempfile.TemporaryDirectory(prefix='efs-host-guard-') as d:
 p=Path(d);src=p/'source';src.mkdir()
 def proc(pid,comm,args,cwd=None):
  q=p/str(pid);q.mkdir();(q/'comm').write_text(comm);(q/'cmdline').write_bytes(('\0'.join(args)+'\0').encode())
  if cwd:(q/'cwd').symlink_to(cwd,target_is_directory=True)
 proc(10,'efsd',[str(src/'efsd'),'store'],src)
 proc(11,'python3',['python3','/data1/efs/w123/tests/live/w123-perf.py'])
 proc(12,'bash',['bash','scripts/make_test_summary.sh'],src)
 proc(13,'gnome-shell',['gnome-shell'])
 proc(14,'efs-fuse',['./efs-fuse','endpoint','mount'],src)
 proc(15,'gcc',['gcc','case.c'])
 proc(16,'python3',['python3','/tmp/old-fixture/tree/efsd','endpoint'])
 proc(17,'python3',['python3','/data1/efs/w123/run-component.py'])
 proc(18,'tar',['tar','-cf','/data1/efs/w123/stores.tar'])
 proc(os.getpid(),'python3',['python3','-',str(p/'efs-artifacts/taskI')])
 # An owned source is allowed during sampling; unrelated traffic/builds are not.
 assert {x['pid'] for x in foreign_jobs(src,proc_root=p)}=={11,15,16,17,18}
 # Preflight owns no workload, including leftovers of the same source.
 assert {x['pid'] for x in foreign_jobs(proc_root=p)}=={10,11,12,14,15,16,17,18}
 # A source path and cwd never adopt another account's process.
 other_uid=os.getuid()+1
 assert {x['pid'] for x in foreign_jobs(src,proc_root=p,current_uid=other_uid)}=={10,11,12,14,15,16,17,18}
 # A known process with missing metadata is not silently classified quiet.
 (p/'15/cmdline').unlink();found=foreign_jobs(src,proc_root=p)
 assert any(x['pid']==15 and 'unreadable' in x.get('reason','') for x in found)
 # A proven zombie is not running work; empty live/unknown processes still
 # block quiet, including when owned-looking path metadata has disappeared.
 for pid,state in ((20,'Z'),(21,'S'),(22,None),(23,'X'),(24,'x'),(25,'R'),(26,'D'),(27,'I')):
  proc(pid,'efs-fuse',[])
  if state:(p/str(pid)/'stat').write_text(f'{pid} (efs-fuse) {state} 1 0')
 found={x['pid'] for x in foreign_jobs(src,proc_root=p)}
 assert not {20,23,24} & found and {21,22,25,26,27} <= found
 found={x['pid'] for x in foreign_jobs(src,proc_root=p,current_uid=other_uid)}
 assert not {20,23,24} & found and {21,22,25,26,27} <= found
 # Record proven ownership without adopting a same-account unknown PID.
 owned=[];rows=foreign_jobs(src,proc_root=p,owned_jobs=owned)
 assert {x['pid'] for x in owned}=={10,12,14}
 assert {21,22,25,26,27}<={x['pid'] for x in rows}
 assert all('state' in x for x in rows if 'command' in x)
print('Task I host guard: all-user EFS/traffic/build discovery, own-source UID proof, unknown metadata refuses quiet PASS')
