#!/usr/bin/env python3
"""Private real-daemon repair protocol boundary; no restored-protection claim."""
import argparse,hashlib,json,os,signal,socket,struct,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--binary',required=True,type=Path);p.add_argument('--build-id',required=True);p.add_argument('--root',required=True,type=Path);a=p.parse_args()
a.binary=a.binary.resolve();a.root.mkdir(exist_ok=False,parents=True)
assert len(a.build_id.encode())<48
identity={'binary':str(a.binary),'sha256':hashlib.sha256(a.binary.read_bytes()).hexdigest(),'build_id':a.build_id,'scope':'real daemon protocol/default-off/incarnation boundary only; no repair or quorum recovery proof'}
(a.root/'identity.json').write_text(json.dumps(identity,indent=2))
env=dict(os.environ,EFS_MD_RAFT_N='3',EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_TRANSPORT='tcp')
for name in ('EFS_REPAIR_EXPERIMENT','EFS_REPAIR_INIT_STORE'):env.pop(name,None)
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
proc=None;log=None
root=a.root/'store'
def read(s,n):
 b=b''
 while len(b)<n:
  piece=s.recv(n-len(b));assert piece,'short reply';b+=piece
 return b
def rpc(kind,body=b''):
 with socket.create_connection(('127.0.0.1',port),timeout=10) as s:
  s.sendall(struct.pack('>IB',len(body)+1,kind)+body);n=struct.unpack('>I',read(s,4))[0];assert n<2048;reply=read(s,n);assert reply[0]==kind+1;return reply[1:]
def rc(reply):return struct.unpack('>i',reply[:4])[0]
def stop():
 global proc,log
 if proc and proc.poll() is None:proc.send_signal(signal.SIGTERM);assert proc.wait(timeout=180)==0
 if log:log.close()
 proc=log=None

def start(name,extra=None,failure=False):
 global proc,log
 extra=extra or {};log=(a.root/(name+'.log')).open('w')
 cmd=[str(a.binary),'--node-id','1','--addr','127.0.0.1','--port',str(port),'--storage',str(root),'--quota','1G']
 proc=subprocess.Popen(cmd,env=dict(env,**extra),stdout=log,stderr=subprocess.STDOUT)
 (a.root/(name+'-pid')).write_text(str(proc.pid))
 if failure:
  code=proc.wait(timeout=30);assert code!=0;log.close();proc=log=None;return
 end=time.monotonic()+60
 while time.monotonic()<end:
  assert proc.poll() is None,'daemon exited'
  try:
   r=rpc(145);assert len(r)==76;return
  except (ConnectionRefusedError,ConnectionResetError):time.sleep(.1)
 raise AssertionError('private listener deadline')

def envelope(build=None,version=256):return struct.pack('>I',version)+(build or a.build_id).encode().ljust(48,b'\0')
try:
 start('default')
 assert rc(rpc(145))==-16 and rc(rpc(147))==-16 and rc(rpc(149))==-16
 assert not (root/'meta/repair-store').exists();stop()
 start('init-without-experiment',{'EFS_REPAIR_INIT_STORE':'1'},failure=True);assert not (root/'meta/repair-store').exists()
 start('experiment-unbound',{'EFS_REPAIR_EXPERIMENT':'1'},failure=True);assert not (root/'meta/repair-store').exists()
 start('experiment-init',{'EFS_REPAIR_EXPERIMENT':'1','EFS_REPAIR_INIT_STORE':'1'})
 assert rc(rpc(145))==-7;assert rc(rpc(145,envelope('different-build')))==-7;assert rc(rpc(145,envelope(version=0)))==-7
 reply=rpc(145,envelope());assert rc(reply)==0 and struct.unpack('>II',reply[4:12])==(1,1) and any(reply[12:28]) and reply[28:].rstrip(b'\0').decode()==a.build_id
 store_id=reply[12:28].hex()
 for body in (b'',envelope('different-build'),envelope()):
  assert rc(rpc(149,body))==-7
 for body,error in ((b'',-7),(envelope('different-build')+b'\1',-7),(envelope()+b'\0',-5),(envelope()+b'\1',-5)):
  reply=rpc(147,body);assert rc(reply)==error and not any(reply[4:]),(rc(reply),error)
 stop();start('experiment-cold',{'EFS_REPAIR_EXPERIMENT':'1'});reply=rpc(145,envelope());assert rc(reply)==0 and reply[12:28].hex()==store_id;stop()
 start('default-cold-refused',failure=True);assert (root/'meta/repair-store').exists()
 saved=(root/'meta/repair-store').read_bytes();(root/'meta/repair-store').unlink()
 start('identity-deleted-off-refused',failure=True);assert (root/'meta/repair-active').exists()
 start('identity-deleted-on-refused',{'EFS_REPAIR_EXPERIMENT':'1'},failure=True)
 (root/'meta/repair-store').write_bytes(b'corrupt')
 start('identity-corrupt-off-refused',failure=True)
 start('identity-corrupt-on-refused',{'EFS_REPAIR_EXPERIMENT':'1'},failure=True)
 (root/'meta/repair-store').write_bytes(saved)
 start('identity-restored-on',{'EFS_REPAIR_EXPERIMENT':'1'});reply=rpc(145,envelope());assert rc(reply)==0 and reply[12:28].hex()==store_id;stop()
 (a.root/'result.json').write_text(json.dumps(dict(status='PASS',store_uuid=store_id,scope=identity['scope'],original_service='not used or modified'),indent=2))
 print('real private daemon: default-off/no implicit identity, invalid setup/build/version/malformed target refusal, zero error receipt, trusted local init and cold UUID, initialized-root default-off restart refused PASS; physical repair NOTRUN')
finally:stop()
