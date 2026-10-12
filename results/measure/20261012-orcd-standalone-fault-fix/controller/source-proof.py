import importlib.util,subprocess,json,hashlib,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
manifest=Path('operations/orcd-fault-fix/qualified-inputs.json')
r=subprocess.run(m.ssh_command('fcstor006','cat > /tmp/efs-qualified-v3-source-manifest.json',m.OPTIONS,m.USER,m.BRIDGE),input=manifest.read_bytes(),capture_output=True,timeout=120);assert r.returncode==0,r.stderr
code=r'''import hashlib,json,pathlib
m=json.loads(pathlib.Path('/tmp/efs-qualified-v3-source-manifest.json').read_text());root=pathlib.Path('/tmp/efs-repair-qualified-ed749cab-v2');bad=[]
for n,want in m['sha256'].items():
 p=root/n
 got=hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None
 if got!=want:bad.append(dict(path=n,expected=want,actual=got))
assert not bad,bad
print(json.dumps(dict(commit=m['commit'],checked_inputs=len(m['sha256']),mismatches=bad,status='PASS')))
'''
r=json.loads(m.shell('fcstor006','python3 -c '+shlex.quote(code),120));Path('operations/orcd-fault-fix/source-proof.json').write_text(json.dumps(r,indent=2)+'\n');print(str(r['checked_inputs'])+' frozen source inputs match ed749cab after qualification PASS')
old=json.loads(Path('operations/orcd-valgrind-fix/runtime.json').read_text());new=json.loads(Path('operations/orcd-fault-fix/runtime.json').read_text());a={r['host']:r for r in old};b={r['host']:r for r in new};assert set(a)==set(b)
fields=('pid','exe_sha256','args','uid')
assert all(a[h]['source']==b[h]['source'] and all(a[h]['processes'][0][f]==b[h]['processes'][0][f] for f in fields) for h in a)
assert all(r['processes'][0]['environment'].get('EFS_REPAIR_EXPERIMENT') in (None,'0') for r in new)
Path('operations/orcd-fault-fix/main-preservation.json').write_text(json.dumps(dict(status='PASS',hosts=len(a),unchanged_fields=fields,source='8b7391b3f3edcf9fc99440d9aad3c240325acb83',experimental_repair='disabled'),indent=2)+'\n');print('all11 installed PIDs/binaries/arguments/UID/source unchanged; experimental repair remains disabled PASS')
