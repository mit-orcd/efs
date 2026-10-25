#!/usr/bin/env python3
"""Task D pure scheduling/health and exact acknowledgement regressions; no cluster."""
import importlib.util
import json
from pathlib import Path
import tempfile
import sys
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('soak', ROOT/'scripts/soak.py')
s=importlib.util.module_from_spec(spec)
spec.loader.exec_module(s)
ev=s.schedule(4*3600)
assert ev==s.schedule(4*3600) and ev!=s.schedule(4*3600, 91)
assert [e['victim'] for e in ev if e['kind']=='add']==[6,7]
assert len([e for e in ev if e['kind']=='crash'])==4
assert len([e for e in ev if e['kind']=='client'])==4
assert len({e['at'] for e in ev})==len(ev)
assert all(0<e['at']<4*3600 for e in ev)
short=s.schedule(900,scale=.05)
assert [e['kind'] for e in short]==[e['kind'] for e in s.schedule(18000)]
assert len({e['victim'] for e in ev[:5] if e['kind']=='clean'})==5
clean='Cluster state: OK (5 up, 0 down)\nDegraded chunks: 0 x\nDegraded segments: 0 x; unrecoverable=0\nProtection: full\nAbandoned uploads: pending=0' 
assert s.clean_protection(s.protection(clean))
# D31 retains active-session tickets until revocation, not until a timer.
assert s.clean_protection(s.protection(clean.replace('pending=0','pending=2')))
assert s.protection(clean.replace('pending=0','pending=2'))['orphans']==2
assert s.clean_protection(s.protection(clean),orphans_revoked=True)
assert not s.clean_protection(s.protection(clean.replace('pending=0','pending=2')),orphans_revoked=True)
assert not s.clean_protection(s.protection(clean.replace('pending=0','pending=unknown')),orphans_revoked=True)
for text in ('',clean.replace('Degraded chunks: 0','Degraded chunks: unknown'),clean.replace('unrecoverable=0','unrecoverable=1'),clean.replace('5 up, 0 down','4 up, 1 down')):
 assert not s.clean_protection(s.protection(text)), text
assert s.recovery_clean('recovery verified missing=2 marked=2 rebuilt=2 rebuild_waiting=0 waiting=0 unrecoverable=0')
for text in ('', 'recovery running', 'recovery verified recovery uncertain:', 'recovery verified ERROR(x)', 'recovery verified waiting=1','recovery verified unrecoverable=1'):
 assert not s.recovery_clean(text), text
with tempfile.TemporaryDirectory() as d:
 req=dict(mount=d,name='x',op='create',token='test',size=8192)
 expected=s.io(req)
 assert expected==s.digest(Path(d)/'x')
 assert s.io(dict(req,op='verify',expected=expected))==expected
 try:s.io(req)
 except FileExistsError:pass
 else:raise AssertionError('create silently replaced existing file')
 expected=s.io(dict(req,op='overwrite',token='new',size=4096))
 assert expected['size']==4096
 trimmed=s.io(dict(req,op='truncate',size=128,token='new'))
 assert trimmed['size']==128
 assert (Path(d)/'x').read_bytes()==s.payload('new',4096)[:128]
 # The oracle must not bless arbitrary bytes merely because readback hashes.
 with open(Path(d)/'x','r+b') as f:f.write(b'!')
 try:s.io(dict(req,op='truncate',size=64,token='new'))
 except AssertionError:pass
 else:raise AssertionError('truncate oracle accepted a corrupted retained prefix')
 for w in (0,1):
  for seq in range(3):s.io(dict(mount=d,name='shared',op='append',writer=w,seq=seq))
 acked={'0':list(range(3)),'1':list(range(3))}
 assert s.io(dict(mount=d,name='shared',op='append-verify',acked=acked))['acknowledged']==6
 try:s.io(dict(req,op='verify',expected=expected))
 except AssertionError:pass
 else:raise AssertionError('corrupt length accepted')
 with open(Path(d)/'shared','r+b') as f:f.write(b'!')
 try:s.io(dict(mount=d,name='shared',op='append-verify',acked=acked))
 except AssertionError:pass
 else:raise AssertionError('corrupt append accepted')
print('soak driver: deterministic schedule, clean-state fail closed, overwrite/truncate exactness, K15/K29 append negative controls PASS')

spec=importlib.util.spec_from_file_location('qualify',ROOT/'scripts/qualify.py')
q=importlib.util.module_from_spec(spec);spec.loader.exec_module(q)
plugins=q.default_plugins({},[])
assert 'soak' in plugins and '{soak_hours}' in plugins['soak']['command']
assert plugins['soak']['deadline_s']>=4*3600
matrix=json.loads((ROOT/'scripts/qualify_matrix.json').read_text())
assert not q.matrix_errors(matrix), q.matrix_errors(matrix)
assert all('soak' in t.get('gates',[]) for t in matrix['topologies'].values() if 'gates' in t)
assert q.default_plugins({'plugins':{'soak':{'command':'site-soak --hours 4'}}},[])['soak']['command']=='site-soak --hours 4'
print('soak qualification plugin: hours parameter, all topology selections, site override and matrix PASS')

short=s.rehearsal_schedule(12345)
assert short==s.rehearsal_schedule(12345) and short!=s.rehearsal_schedule(42)
assert len(short)==9 and short[-1]['at']<900
assert sorted(e['victim'] for e in short if e['kind']=='clean')==[1,2,3,4,5]
assert {e['kind'] for e in short}=={'clean','crash','client','add'}
assert len({e['at'] for e in short})==len(short)
print('15-minute representative lifecycle schedule PASS')

u='ab'*16
stamp='2026-10-25T16:34:48.908Z '
assert s.owned_session(f'{stamp}session uuid={u} epoch=12\n')==(u,12)
for text in ('',f'{stamp}session uuid={u[:-1]} epoch=1\n',f'{stamp}xsession uuid={u} epoch=1',f'{stamp}session uuid={u} epoch=1\n{stamp}session uuid={u} epoch=2\n',f'bad-timestamp session uuid={u} epoch=1\n'):
 try:s.owned_session(text)
 except AssertionError:pass
 else:raise AssertionError('ambiguous/unowned session accepted')
print('D31 owned-session identity and active-versus-revoked collection controls PASS')

# Force the former spawn-versus-/proc ownership race: the native child exists
# before Cluster.start publishes its PID. The observer must wait for enrollment.
import threading,types
h=s.Soak.__new__(s.Soak);h.mu=threading.RLock()
spawned,release,observed=threading.Event(),threading.Event(),threading.Event()
class Nodes:
 def __init__(self):self.procs={}
 def start(self,i,**kwargs):
  spawned.set();assert release.wait(2)
  self.procs[i]=types.SimpleNamespace(pid=4242)
h.c=Nodes();old=s.foreign_jobs
errors=[]
def scan(owned=()):
 observed.set();assert list(owned)==[4242], 'guard observed an unenrolled owned child'
 return []
s.foreign_jobs=scan
starter=threading.Thread(target=lambda:h.start_node(1));starter.start();assert spawned.wait(2)
def probe():
 try:assert h.unrelated_jobs()==[]
 except BaseException as e:errors.append(e)
observer=threading.Thread(target=probe);observer.start()
assert not observed.wait(.05), 'ownership scan ran inside an incomplete spawn'
release.set();starter.join(2);observer.join(2);s.foreign_jobs=old
assert not starter.is_alive() and not observer.is_alive() and not errors,errors
assert observed.is_set()
print('soak ownership registry: forced child-birth/observer interleaving PASS')

# Current operator contract: bounded cleanup is explicitly resumable, and all
# touched shards must be completed after a parallel coordinator stops early.
def view(shard=4,epoch=2,state=1,bit=0):
 return f'raft-session status=0 rc=0 primary=3 op=get ssh=7 shard={shard} epoch={epoch} state={state} touched={bit} index=99\n'
busy=('session cleanup incomplete: epoch=1 shard=1 rc=-13; resume cleanup-leases and cleanup-appends for that epoch/shard\n'
      'session fence incomplete: phase=cleanup epoch=2 rc=-13\n')
calls=[];counts={};tick=[0.]
def clock():return tick[0]
def pause(n):tick[0]+=n
def invoke(op,uuid,*args,timeout):
 assert uuid==u and 0<timeout<=60
 calls.append((op,args));tick[0]+=.0001
 if op=='fence':return 1,busy
 if op=='get':return 0,view(int(args[0]) if args else 4,bit=int(bool(args) and int(args[0]) in (1,7,2049)))
 epoch,shard=map(int,args);assert epoch==1
 key=(op,shard);counts[key]=counts.get(key,0)+1
 rc=-13 if key==('cleanup-appends',7) and counts[key]==1 else 0
 return int(rc!=0),f'raft-session op={op} epoch={epoch} shard={shard} rc={rc} index=99 salt={0 if rc else 1}\n'
proof=s.revoke_owned_session(invoke,u,1,300,clock,pause)
assert proof['resumed'] and proof['touched']==[1,7,2049]
assert proof['cleanup_invocations']==7 and sum(op=='fence' for op,_ in calls)==1
assert len([1 for op,args in calls if op=='get' and args])==4096
for bad in (busy.replace('phase=cleanup','phase=storage-fence'),busy.replace('epoch=1 shard=1','epoch=9 shard=1'),'network error',busy.replace('rc=-13','rc=-5')):
 def reject(op,uuid,*args,timeout):assert op=='fence';return 1,bad
 try:s.revoke_owned_session(reject,u,1,300)
 except AssertionError:pass
 else:raise AssertionError('unproven cleanup result resumed')
# No extra budget, no epoch drift, no empty-phase inference from BUSY.
for mode in ('deadline','epoch','wrong-shard','io','not-empty'):
 calls.clear();counts.clear();tick[0]=0.
 def faulty(op,uuid,*args,timeout):
  if op=='get' and mode=='epoch':return 0,view(epoch=3)
  if op.startswith('cleanup'):
   if mode=='deadline':tick[0]=301.;return 1,f'raft-session op={op} epoch=1 shard={args[1]} rc=-13 index=0 salt=0\n'
   if mode=='wrong-shard':return 0,f'raft-session op={op} epoch=1 shard=888 rc=0 index=99 salt=1\n'
   if mode=='io':return 1,f'raft-session op={op} epoch=1 shard={args[1]} rc=-5 index=0 salt=0\n'
   if mode=='not-empty':return 0,f'raft-session op={op} epoch=1 shard={args[1]} rc=0 index=99 salt=0\n'
  return invoke(op,uuid,*args,timeout=timeout)
 try:s.revoke_owned_session(faulty,u,1,300,clock,pause)
 except AssertionError:pass
 else:raise AssertionError(f'unsafe cleanup accepted: {mode}')
print('owned session resume: frozen epoch, all touched shards, BUSY completion, no re-fence and negative controls PASS')
