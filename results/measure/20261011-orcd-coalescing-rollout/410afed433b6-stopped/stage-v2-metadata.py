import importlib.util,concurrent.futures,json
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
base=m.BULK+'/qualification-410afed433b6-v2';old=m.BULK+'/qualification-410afed433b6';src='/tmp/efs-orcd-stage-410afed433b6'
def one(h):
 body=f'''cd {base}
echo 'e5685d586e1de93335d1ff339f22cb24ba64600949980b3386e1b07f556d732e  source.tar.gz
8939a4e679f5a431e4fb72a76248abc1867b5ec45a19fdf3d1e7c921f0b7bee6  inputs.json
272154ba5370b5c23d5defaef1e4f497c4be4fbc32daf1dea788f4d0809a3110  dirs.json' | sha256sum -c -
cd {src};sha256sum -c {old}/{h}-stage-binaries.sha256
python3 - <<'CHECK'
import pathlib,json,hashlib,tarfile
root=pathlib.Path({src!r});new=pathlib.Path({base!r});old=pathlib.Path({old!r});rows=json.loads((new/'inputs.json').read_text());prior=json.loads((old/'inputs.json').read_text())
assert len(rows)==1563 and all(rows[n]==v for n,v in prior.items())
assert all(hashlib.sha256((root/n).read_bytes()).hexdigest()==v for n,v in prior.items())
with tarfile.open(new/'source.tar.gz') as t:
 for n in set(rows)-set(prior):
  p=root/n;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(t.extractfile(n).read())
assert all(hashlib.sha256((root/n).read_bytes()).hexdigest()==v for n,v in rows.items())
CHECK
python3 docs/check-architecture.py > {base}/{h}-stage-v2-docgate.log
sha256sum -c {old}/{h}-stage-binaries.sha256
python3 - <<'CHECK'
import pathlib,json,hashlib
root=pathlib.Path({src!r});base=pathlib.Path({base!r});rows=json.loads((base/'inputs.json').read_text());bad=[n for n,h in rows.items() if hashlib.sha256((root/n).read_bytes()).hexdigest()!=h];assert not bad,bad
(base/{(h+'-stage-v2-inputs.json')!r}).write_text(json.dumps(dict(source='410afed433b6da093bdc2acf37bec253ddbd586c',inputs=len(rows),mismatches=bad,binaries_retained=True,docs='PASS')))
CHECK
'''
 try:return dict(host=h,result='PASS',output=m.shell(h,body,timeout=300))
 except Exception as e:return dict(host=h,result='FAIL',error=str(e))
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:rows=list(pool.map(one,[h for h in m.HOSTS if h!='fcstor006']))
Path('operations/orcd-410a-qualification/stage-v2-verification.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(rows),flush=True);assert all(r['result']=='PASS' for r in rows)
