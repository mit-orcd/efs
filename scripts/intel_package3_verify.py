#!/usr/bin/env python3
"""Run on each Intel host after package 3 deployment; inspect actual /proc state."""
import os,pathlib,json,subprocess
import pwd
assert os.getuid()!=0 and pwd.getpwuid(os.getuid()).pw_name=='efs'
root=pathlib.Path('/data1/efs'); rows=[]
for proc in pathlib.Path('/proc').iterdir():
 if not proc.name.isdigit():continue
 try:
  args=[x.decode() for x in (proc/'cmdline').read_bytes().split(b'\0') if x]
  if not args or pathlib.Path(args[0]).name not in ('efsd','efs-fuse'):continue
  env=dict(x.decode().split('=',1) for x in (proc/'environ').read_bytes().split(b'\0') if b'=' in x)
  assert proc.stat().st_uid==os.getuid()
  assert env.get('EFS_TRANSPORT')=='tcp',(args,env.get('EFS_TRANSPORT'))
  for k in ('EFS_COMMANDS','EFS_COMMANDS_SERVER','EFS_EXPIRY'):assert env.get(k)=='0',(args,k,env.get(k))
  assert pathlib.Path(os.readlink(proc/'exe')).parent==root/'src',args
  if pathlib.Path(args[0]).name=='efsd':
   assert '--direct-io' in args,args
   assert args[args.index('--quota')+1]=='400G'
  else:assert args[3]==str(root/'mnt'),args
  rows.append({'pid':int(proc.name),'args':args,'features':{k:env[k] for k in ('EFS_TRANSPORT','EFS_COMMANDS','EFS_COMMANDS_SERVER','EFS_EXPIRY')}})
 except (FileNotFoundError,ProcessLookupError,PermissionError):continue
assert len(rows)==3,rows
assert os.path.ismount(root/'mnt')
assert not (root/'mnt/.efs').exists(),'virtual commands unexpectedly visible'
policy=subprocess.check_output([str(root/'src/efs-mgmt'),'commands-policy','10.0.0.207:17432','get'],text=True,timeout=30)
assert 'rc=0 ' in policy and 'mask=0 ' in policy,policy
print(json.dumps({'status':'PASS','processes':rows,'policy':policy.strip()}))
