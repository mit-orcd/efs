import importlib.util,concurrent.futures,json,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
base=m.BULK+'/qualification-release-28b2089be6a5';src='/tmp/efs-orcd-release-28b2089be6a5';meta=json.loads(Path('operations/orcd-release-28b2089b/bundle.json').read_text())
def one(h):
 verify='\n'.join('echo '+shlex.quote(v+'  '+n)+' | sha256sum -c -' for n,v in meta['sha256'].items())
 body=f'''cd {base}
{verify}
cd {src};sha256sum -c {base}/{h}-stage-binaries.sha256
python3 - <<'CHECK'
import pathlib,json,hashlib,tarfile
root=pathlib.Path({src!r});base=pathlib.Path({base!r});rows=json.loads((base/'inputs.json').read_text());assert len(rows)==1588
assert all(hashlib.sha256((root/n).read_bytes()).hexdigest()==h for n,h in rows.items())
with tarfile.open(base/'source.tar.gz') as t:
 for m in t.getmembers():
  assert m.mtime>=315532800
  import os;os.utime(root/m.name,(m.mtime,m.mtime))
CHECK
python3 docs/check-architecture.py > {base}/{h}-stage-docgate.log
python3 tests/test_bench_profile.py > {base}/{h}-timestamp-profile-check.log 2>&1
sha256sum -c {base}/{h}-stage-binaries.sha256
python3 - <<'CHECK'
import pathlib,json,hashlib
root=pathlib.Path({src!r});base=pathlib.Path({base!r});rows=json.loads((base/'inputs.json').read_text());bad=[n for n,h in rows.items() if hashlib.sha256((root/n).read_bytes()).hexdigest()!=h];assert not bad,bad
(base/{(h+'-final-stage-inputs.json')!r}).write_text(json.dumps(dict(source={meta['source']!r},inputs=len(rows),mismatches=bad,binaries_retained=True,docs='PASS',profile_timestamp_check='PASS')))
CHECK
'''
 try:return dict(host=h,result='PASS',output=m.shell(h,body,timeout=300))
 except Exception as e:return dict(host=h,result='FAIL',error=str(e))
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:rows=list(pool.map(one,[h for h in m.HOSTS if h!='fcstor006']))
Path('operations/orcd-release-28b2089b/final-stage-verification.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(rows),flush=True);assert all(r['result']=='PASS' for r in rows)
