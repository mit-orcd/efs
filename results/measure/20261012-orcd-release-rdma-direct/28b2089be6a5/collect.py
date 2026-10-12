import importlib.util,subprocess,tarfile,io,json,shutil,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3'];base=m.BULK+'/qualification-release-28b2089be6a5';repo=Path('work/devel-orcd');out=repo/'results/measure/20261012-orcd-release-rdma-direct/28b2089be6a5';out.mkdir(parents=True,exist_ok=True)
def download(command,target):
 r=subprocess.run(m.ssh_command('fcstor006',command,m.OPTIONS,m.USER,m.BRIDGE),stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=180);assert r.returncode==0,r.stderr
 target.mkdir(parents=True,exist_ok=True)
 with tarfile.open(fileobj=io.BytesIO(r.stdout),mode='r:gz') as t:
  assert all(not Path(x.name).is_absolute() and '..' not in Path(x.name).parts and x.isfile() for x in t.getmembers());t.extractall(target)
cmd=f"find {base} -type f ! -path '{base}/real-lsm-sanitizer-src/*' ! -path '{base}/normal-binaries/*' ! -path '{base}/fault-binaries/*' ! -name source.tar.gz -printf '%P\\0' | tar -czf - -C {base} --null -T -"
download(cmd,out/'qualification')
roots=['/data1/01/efs-orcd-release-28b2089be6a5-private','/data1/02/efs-orcd-release-28b2089be6a5-private']
for index,root in enumerate(roots,1):
 cmd=f"find {root} -maxdepth 3 -type f \\( -name '*.log' -o -name '*.json' -o -name '*.tsv' -o -name '*.txt' -o -name '*.jsonl' -o -name '*.rc' -o -name '*.exit' \\) -printf '%P\\0' | tar -czf - -C {root} --null -T -"
 download(cmd,out/('private-root-'+str(index)))
root='/tmp/efs-fault-release-28b2089be6a5';cmd=f"find {root} -maxdepth 3 -type f \\( -name '*.log' -o -name '*.json' -o -name '*.jsonl' \\) -printf '%P\\0' | tar -czf - -C {root} --null -T -";download(cmd,out/'fault-integration')
ops=Path('operations/orcd-release-28b2089b')
for p in ops.iterdir():
 if p.suffix in ('.log','.py','.sh','.json'):shutil.copy2(p,out/p.name)
print('raw suites, manifests, physical/cold/fault witnesses andrecipes retained',flush=True)
