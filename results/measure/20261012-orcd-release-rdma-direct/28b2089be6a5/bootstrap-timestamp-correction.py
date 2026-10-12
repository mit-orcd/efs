import importlib.util,subprocess,tarfile,io,json,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
p=Path('operations/orcd-release-28b2089b');meta=json.loads((p/'bundle.json').read_text());base=m.BULK+'/qualification-release-28b2089be6a5';src='/tmp/efs-orcd-release-28b2089be6a5'
buf=io.BytesIO()
with tarfile.open(fileobj=buf,mode='w') as t:
 for f in ['source.tar.gz','inputs.json','dirs.json','bundle.json','full-prerequisites.sh']:t.add(p/f,arcname=f)
r=subprocess.run(m.ssh_command('fcstor006','mkdir -p '+base+' && tar -xf - -C '+base,m.OPTIONS,m.USER,m.BRIDGE),input=buf.getvalue(),capture_output=True);assert r.returncode==0,r.stderr
print('exact branch bundle uploaded',flush=True)
verify='\n'.join('echo '+shlex.quote(h+'  '+n)+' | sha256sum -c -' for n,h in meta['sha256'].items())
body=f'''cd {base}
{verify}
test -d {src}
test ! -e {base}/pre1980-packaging-failure;mkdir {base}/pre1980-packaging-failure
find {base} -maxdepth 1 -type f \( -name '*.log' -o -name '*.exit' \) -exec mv {{}} {base}/pre1980-packaging-failure/ \;
tar -xzf source.tar.gz -C {src}
python3 - <<'CHECK'
import pathlib,json,hashlib
base=pathlib.Path({base!r});root=pathlib.Path({src!r})
for n in json.loads((base/'dirs.json').read_text()):(root/n).mkdir(parents=True,exist_ok=True)
rows=json.loads((base/'inputs.json').read_text());bad=[n for n,h in rows.items() if hashlib.sha256((root/n).read_bytes()).hexdigest()!=h];assert not bad,bad
(base/'pre-native-input-verification.json').write_text(json.dumps(dict(source={meta['source']!r},inputs=len(rows),mismatches=bad)))
CHECK
bash full-prerequisites.sh
python3 - <<'CHECK'
import pathlib,json,hashlib
base=pathlib.Path({base!r});root=pathlib.Path({src!r});rows=json.loads((base/'inputs.json').read_text());bad=[n for n,h in rows.items() if hashlib.sha256((root/n).read_bytes()).hexdigest()!=h];assert not bad,bad
(base/'post-native-input-verification.json').write_text(json.dumps(dict(source={meta['source']!r},inputs=len(rows),mismatches=bad)))
print('full release normal/fault/sanitizer prerequisites PASS')
CHECK
'''
print(m.shell('fcstor006',body,timeout=7200),flush=True)
