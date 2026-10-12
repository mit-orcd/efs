import importlib.util,concurrent.futures,json,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3'];b=m.BULK+'/qualification-release-28b2089be6a5'
def one(h):
 code=f'''import pathlib,os,json,hashlib,subprocess
root=pathlib.Path('/tmp/efs');base=pathlib.Path({b!r});rows=json.loads((base/'inputs.json').read_text());bad=[n for n,v in rows.items() if hashlib.sha256((root/n).read_bytes()).hexdigest()!=v];assert not bad,bad
active=[]
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdigit() or int(p.name)==os.getpid():continue
 try:
  if p.stat().st_uid!=os.getuid():continue
  a=(p/'cmdline').read_bytes().replace(bytes([0]),b' ').decode();name=(p/'comm').read_text().strip()
  if name in ('efsd','efs-fuse') and os.readlink(p/'exe')=='/tmp/efs/'+name:
   if name=='efsd' and '--port 17432' not in a:active.append(dict(pid=p.name,args=a))
   if name=='efs-fuse' and '/tmp/efs-mount' not in a:active.append(dict(pid=p.name,args=a))
  if name in ('python3','python') and any(x in a for x in ['tests/live/gc_reclamation.py','tests/faults/harness.py','tests/posix/posix_suite.py','posix_2client.py --exec','tests/posix/posix_persist.py']):active.append(dict(pid=p.name,args=a))
 except (FileNotFoundError,PermissionError,ProcessLookupError):pass
assert not active,active
mounts=subprocess.run(['findmnt','-rn','-t','fuse.efs-fuse','-o','TARGET'],capture_output=True,text=True).stdout.splitlines();assert all(x=='/tmp/efs-mount' for x in mounts),mounts
print(json.dumps(dict(host={h!r},inputs=len(rows),mismatches=bad,owned_test_processes=active,mounts=mounts,prior_source_retained=pathlib.Path('/tmp/efs-before-release-28b2089be6a5').is_dir())))'''
 r=json.loads(m.shell(h,'python3 -c '+shlex.quote(code),timeout=60));assert r['prior_source_retained']
 manifest='normal-binary-sha256.txt' if h=='fcstor006' else h+'-stage-binaries.sha256';r['binaries_retained']=m.shell(h,'cd /tmp/efs; sha256sum -c '+b+'/'+manifest,timeout=60);return r
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:rows=list(pool.map(one,m.HOSTS))
Path('operations/orcd-release-28b2089b/final-source-and-cleanup.json').write_text(json.dumps(rows,indent=2)+'\n');print('all11 source1588/normalbinaryhashes preserved, prior source backups retained, no test processes/private mounts PASS')
