import importlib.util,shlex,json
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
code=r'''import pathlib,os,subprocess,json
prefixes=('/tmp/efs-fault-fix-','/tmp/efs-fault-repair-','/tmp/efs-repair-','/tmp/efs-qualified-')
processes=[]
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  if p.stat().st_uid!=os.getuid():continue
  exe=os.readlink(p/'exe');name=(p/'comm').read_text().strip()
  if name in ('efsd','efs-fuse') and exe.startswith(prefixes):processes.append(dict(pid=int(p.name),exe=exe))
 except (FileNotFoundError,PermissionError,ProcessLookupError):pass
mounts=[x for x in subprocess.check_output(['findmnt','-rn','-o','TARGET'],text=True).splitlines() if x.startswith(prefixes)]
assert not processes and not mounts,(processes,mounts)
print(json.dumps(dict(host='fcstor006',uid=os.getuid(),owned_processes=processes,owned_mounts=mounts,status='PASS')))
'''
r=json.loads(m.shell('fcstor006','python3 -c '+shlex.quote(code),60));Path('operations/orcd-fault-fix/cleanup.json').write_text(json.dumps(r,indent=2)+'\n');print('all owned private daemon/client processes and mounts retired PASS')
