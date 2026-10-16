import importlib.util,concurrent.futures,json,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
base=m.BULK+'/qualification-c2bfb9e9a281';stage='/tmp/efs-orcd-stage-c2bfb9e9a281'
def build(host):
 body=f'''d={shlex.quote(base)}
s={shlex.quote(stage)}
test ! -e "$s"
mkdir "$s"
cd "$d"
echo '4144442ef9c4733d254b7506496022ce614068eeb1eed452745fb7d61d6638ec  source.tar.gz' | sha256sum -c -
tar -xzf source.tar.gz -C "$s"
python3 - <<'CHECK'
import pathlib,json,hashlib
base=pathlib.Path({base!r});root=pathlib.Path({stage!r})
for name in json.loads((base/'dirs.json').read_text()):(root/name).mkdir(parents=True,exist_ok=True)
rows=json.loads((base/'inputs.json').read_text());bad=[n for n,h in rows.items() if hashlib.sha256((root/n).read_bytes()).hexdigest()!=h];assert not bad,bad
CHECK
cd "$s"
export PKG_CONFIG_PATH={m.BULK}/deps/libfuse/lib/pkgconfig:${{PKG_CONFIG_PATH:-}}
make -j4 EFS_GIT_ID=c2bfb9e9-release-normal EFS_GIT_BRANCH=release-v0.3.0-pre-alpha EFS_GIT_DESCRIBE=v0.3.0-pre-alpha-candidate FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,{m.BULK}/deps/libfuse/lib" all > "$d/{host}-stage-build.log" 2>&1
sha256sum efsd efs-fuse efs-mgmt libefs.a > "$d/{host}-stage-binaries.sha256"
python3 - <<'CHECK'
import pathlib,json,hashlib
base=pathlib.Path({base!r});root=pathlib.Path({stage!r});rows=json.loads((base/'inputs.json').read_text());bad=[n for n,h in rows.items() if hashlib.sha256((root/n).read_bytes()).hexdigest()!=h];assert not bad,bad
(base/{(host+'-stage-inputs.json')!r}).write_text(json.dumps(dict(inputs=len(rows),mismatches=bad)))
CHECK
echo stage-build-PASS
'''
 try:return dict(host=host,result='PASS',output=m.shell(host,body,timeout=3600))
 except Exception as e:return dict(host=host,result='FAIL',error=str(e))
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
 rows=[]
 for row in pool.map(build,['fcstor003','fcstor004']):
  rows.append(row);print(json.dumps(row),flush=True)
Path('operations/orcd-c2bf-qualification/stage-builds-retry.json').write_text(json.dumps(rows,indent=2))
assert all(r['result']=='PASS' for r in rows)
