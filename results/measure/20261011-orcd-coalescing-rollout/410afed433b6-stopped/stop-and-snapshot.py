import importlib.util,shlex,json
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
code='''import pathlib,os,json,signal,time
src='/tmp/efs-orcd-unit-410afed4';found={}
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  if p.stat().st_uid!=os.getuid():continue
  cmd=(p/'cmdline').read_bytes().split(bytes([0]));status=(p/'status').read_text();ppid=int(next(l.split()[1] for l in status.splitlines() if l.startswith('PPid:')))
  found[int(p.name)]=dict(pid=int(p.name),ppid=ppid,args=[x.decode(errors='replace') for x in cmd if x],cwd=os.readlink(p/'cwd'))
 except (FileNotFoundError,PermissionError,ProcessLookupError):pass
roots=[p for p,r in found.items() if r['cwd']==src and any(x=='full-prerequisites.sh' or x.endswith('/full-prerequisites.sh') for x in r['args'])]
owned=set(roots)
while True:
 new={p for p,r in found.items() if r['ppid'] in owned};n=new-owned
 if not n:break
 owned|=n
before=[found[p] for p in sorted(owned)]
for p in roots:
 try:os.kill(p,signal.SIGTERM)
 except ProcessLookupError:pass
for p in owned-set(roots):
 try:os.kill(p,signal.SIGTERM)
 except ProcessLookupError:pass
time.sleep(2)
alive=[]
for p in owned:
 try:
  stat=pathlib.Path('/proc',str(p),'stat').read_text();state=stat.split(') ',1)[1].split()[0]
  if state!='Z':alive.append(p)
 except FileNotFoundError:pass
assert not alive,alive
print(json.dumps(dict(stop_reason='direct human manager instruction',owned_before=before,active_after=alive)))
'''
r=json.loads(m.shell('fcstor006','python3 -c '+shlex.quote(code),timeout=30));Path('operations/orcd-410a-qualification/stopped-processes.json').write_text(json.dumps(r,indent=2)+'\n');print('owned qualification processes stopped',len(r['owned_before']))
