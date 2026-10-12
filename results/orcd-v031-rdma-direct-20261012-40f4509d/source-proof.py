from pathlib import Path
import subprocess,tarfile,io,hashlib,json,importlib.util,concurrent.futures,shlex,sys
p=Path('operations/orcd-v031-rdma');meta=json.loads((p/'profile-stage.json').read_text());rev=meta['revision'];repo='/Users/mike/Documents/Codex/2026-10-08/efs/work/fix-orcd-valgrind'
data=subprocess.check_output(['git','-C',repo,'archive',rev,'src','include','scripts','tests','deps','Makefile'])
with tarfile.open(fileobj=io.BytesIO(data)) as t:files={m.name:hashlib.sha256(t.extractfile(m).read()).hexdigest() for m in t if m.isfile()}
(p/'code-inputs.json').write_text(json.dumps({'revision':rev,'files':files},indent=2)+'\n')
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
remote=m.BULK+'/v031-code-inputs-'+rev[:12]+'.json'
subprocess.run(['ssh','-A','-o','ControlPath=/private/tmp/efs-orcd-login-control3',m.BRIDGE,'cat > '+remote],input=(p/'code-inputs.json').read_bytes(),check=True)
source='/tmp/efs' if '--installed' in sys.argv else meta['stage']
def one(host):
 code=f'''from pathlib import Path
import json,hashlib
m=json.loads(Path({remote!r}).read_text());root=Path({source!r});bad=[name for name,sha in m['files'].items() if not (root/name).is_file() or hashlib.sha256((root/name).read_bytes()).hexdigest()!=sha]
assert not bad,bad
print(json.dumps({{'host':{host!r},'revision':m['revision'],'files':len(m['files']),'mismatches':bad}}))
'''
 return json.loads(m.shell(host,'python3 -c '+shlex.quote(code),120))
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:rows=list(pool.map(one,m.HOSTS))
(p/('source-proof-installed.json' if '--installed' in sys.argv else 'source-proof-stage.json')).write_text(json.dumps(rows,indent=2)+'\n')
print('all11',len(files),'code/test input hashes per host verified')
