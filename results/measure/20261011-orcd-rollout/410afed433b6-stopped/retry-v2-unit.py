import importlib.util,subprocess,tarfile,io,json,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
base=m.BULK+'/qualification-410afed433b6-v2';src='/tmp/efs-orcd-unit-410afed4'
buf=io.BytesIO()
with tarfile.open(fileobj=buf,mode='w') as t:
 for a,b in [('/tmp/efs-v030-410afed4-source-v2.tar.gz','source.tar.gz'),('/tmp/v030-410afed4-inputs-v2.json','inputs.json'),('/tmp/v030-410afed4-dirs.json','dirs.json'),('operations/orcd-410a-qualification/full-prerequisites.sh','full-prerequisites.sh')]:t.add(a,arcname=b)
p=subprocess.run(m.ssh_command('fcstor006','mkdir -p '+shlex.quote(base)+' && tar -xf - -C '+shlex.quote(base),m.OPTIONS,m.USER,m.BRIDGE),input=buf.getvalue(),capture_output=True);assert p.returncode==0,p.stderr
print('bundle uploaded',flush=True)
body=f'''cd {base}
echo 'e5685d586e1de93335d1ff339f22cb24ba64600949980b3386e1b07f556d732e  source.tar.gz
8939a4e679f5a431e4fb72a76248abc1867b5ec45a19fdf3d1e7c921f0b7bee6  inputs.json
272154ba5370b5c23d5defaef1e4f497c4be4fbc32daf1dea788f4d0809a3110  dirs.json' | sha256sum -c -
test -d {src}
old={m.BULK}/qualification-410afed433b6
test ! -e "$old/original-packaging-failure";mkdir "$old/original-packaging-failure"
find "$old" -maxdepth 1 -type f \( -name '*.log' -o -name '*.exit' \) -exec mv {{}} "$old/original-packaging-failure/" \;
tar -xzf source.tar.gz -C {src}
cp full-prerequisites.sh "$old/full-prerequisites.sh"
python3 - <<'CHECK'
import pathlib,json,hashlib
base=pathlib.Path({base!r});root=pathlib.Path({src!r})
for d in json.loads((base/'dirs.json').read_text()):(root/d).mkdir(parents=True,exist_ok=True)
rows=json.loads((base/'inputs.json').read_text());assert len(rows)==1563
bad=[n for n,h in rows.items() if hashlib.sha256((root/n).read_bytes()).hexdigest()!=h];assert not bad,bad
(base/'pre-native-input-verification.json').write_text(json.dumps(dict(source='410afed433b6da093bdc2acf37bec253ddbd586c',inputs=len(rows),mismatches=bad)))
CHECK
bash full-prerequisites.sh
python3 - <<'CHECK'
import pathlib,json,hashlib
base=pathlib.Path({base!r});root=pathlib.Path({src!r});rows=json.loads((base/'inputs.json').read_text());bad=[n for n,h in rows.items() if hashlib.sha256((root/n).read_bytes()).hexdigest()!=h];assert not bad,bad
(base/'post-native-input-verification.json').write_text(json.dumps(dict(source='410afed433b6da093bdc2acf37bec253ddbd586c',inputs=len(rows),mismatches=bad)))
print('full410prerequisites PASS')
CHECK
'''
print(m.shell('fcstor006',body,timeout=7200),flush=True)
