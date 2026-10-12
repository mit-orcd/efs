from pathlib import Path
import json,tarfile,hashlib,subprocess,time
b=Path('/data1/efs/intel-e59-focused-20261012');out=b/'evidence';out.mkdir();root=b/'normal';root.mkdir()
manifest=json.loads((b/'inputs.json').read_text())
with tarfile.open(b/'source.tar.gz') as t:
 assert all(not Path(m.name).is_absolute() and '..' not in Path(m.name).parts for m in t.getmembers());t.extractall(root)
bad=[n for n,h in manifest.items() if hashlib.sha256((root/n).read_bytes()).hexdigest()!=h];assert not bad,bad
(out/'source-proof.json').write_text(json.dumps({'source':'e59e815fab2916340baad585dc08712ed182b7ea','all_match':True,'files':len(manifest)},indent=2))
cmd=['make','-j4','EFS_FAULTS=0','EFS_GIT_ID=e59e815fab2916340baad585dc08712ed182b7ea','EFS_GIT_BRANCH=qualification-v030-tcp-direct','efsd','efs-fuse','efs-mgmt','efs-query','efs-bench','libefs.a','tests/test_raft']
with (out/'build.log').open('w') as f:r=subprocess.run(cmd,cwd=root,stdout=f,stderr=subprocess.STDOUT)
assert r.returncode==0,r.returncode
record={'command':cmd,'returncode':r.returncode,'binaries':{n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in ('efsd','efs-fuse','efs-mgmt','efs-query','efs-bench','libefs.a')},'version':subprocess.check_output([str(root/'efsd'),'--version'],text=True)};(out/'build.json').write_text(json.dumps(record,indent=2))
(out/'builds.finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
