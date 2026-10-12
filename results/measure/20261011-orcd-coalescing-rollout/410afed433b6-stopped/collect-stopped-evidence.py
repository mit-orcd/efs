import importlib.util,subprocess,tarfile,io,json,hashlib,shutil,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
base=m.BULK+'/qualification-410afed433b6';out=Path('work/devel-orcd/results/measure/20261011-orcd-coalescing-rollout/410afed433b6-stopped');out.mkdir(exist_ok=True)
cmd="find "+base+" -maxdepth 2 -type f ! -name source.tar.gz ! -path '*/fault-binaries/*' ! -path '*/normal-binaries/*' -printf '%P\\0' | tar -czf - -C "+base+" --null -T -"
p=subprocess.run(m.ssh_command('fcstor006',cmd,m.OPTIONS,m.USER,m.BRIDGE),stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=120);assert p.returncode==0,p.stderr
raw=out/'remote-qualification';raw.mkdir(exist_ok=True)
with tarfile.open(fileobj=io.BytesIO(p.stdout),mode='r:gz') as t:
 assert all(not Path(x.name).is_absolute() and '..' not in Path(x.name).parts and x.isfile() for x in t.getmembers());t.extractall(raw)
cmd="find "+base+"-v2 -maxdepth 1 -type f ! -name source.tar.gz -printf '%f\\0' | tar -czf - -C "+base+"-v2 --null -T -"
p=subprocess.run(m.ssh_command('fcstor006',cmd,m.OPTIONS,m.USER,m.BRIDGE),stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=120);assert p.returncode==0,p.stderr
raw2=out/'remote-v2-verification';raw2.mkdir(exist_ok=True)
with tarfile.open(fileobj=io.BytesIO(p.stdout),mode='r:gz') as t:
 assert all(not Path(x.name).is_absolute() and '..' not in Path(x.name).parts and x.isfile() for x in t.getmembers());t.extractall(raw2)
code='''import pathlib,json,hashlib,os
src=pathlib.Path('/tmp/efs-orcd-unit-410afed4');base=pathlib.Path('/home/erbmi1/orcd/scratch/efs/orcd-test/qualification-410afed433b6-v2');rows=json.loads((base/'inputs.json').read_text());bad=[n for n,h in rows.items() if hashlib.sha256((src/n).read_bytes()).hexdigest()!=h];assert not bad,bad
active=[]
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  if p.stat().st_uid==os.getuid() and os.readlink(p/'cwd')==str(src):active.append(dict(pid=p.name,args=(p/'cmdline').read_bytes().replace(bytes([0]),b' ').decode()))
 except (FileNotFoundError,PermissionError,ProcessLookupError):pass
assert not active,active
print(json.dumps(dict(source='410afed433b6da093bdc2acf37bec253ddbd586c',inputs=len(rows),mismatches=bad,active_unit_processes=active)))'''
r=json.loads(m.shell('fcstor006','python3 -c '+shlex.quote(code),timeout=60));(out/'post-stop-input-verification.json').write_text(json.dumps(r,indent=2)+'\n')
for x in Path('operations/orcd-410a-qualification').iterdir():
 if x.suffix in ('.log','.py','.sh','.json'):shutil.copy2(x,out/x.name)
print('410 stopped evidence retained; post-stop1563 unchanged/no active unit process')
