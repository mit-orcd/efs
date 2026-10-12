import importlib.util,concurrent.futures,json,shlex
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3'];b=m.BULK+'/qualification-release-28b2089be6a5/suites'
def one(h):
 code=f'''from pathlib import Path
import json,collections
p=Path({(b+'/'+h)!r});f=p/'mounted-posix.tsv';lines=f.read_text().splitlines() if f.exists() else [];rows=[r.split('\\t') for r in lines if r and not r.startswith('#') and not r.startswith('test\\t')];counts=collections.Counter(r[1] for r in rows if len(r)>1);bad=[dict(test=r[0],status=r[1],detail=r[-1][:180]) for r in rows if len(r)>1 and r[1].upper() not in ('PASS','SKIP')]
print(json.dumps(dict(host={h!r},counts=dict(counts),complete=any(r.startswith('# complete') for r in lines),exit=(p/'mounted-posix.exit').read_text().strip() if (p/'mounted-posix.exit').exists() else None,bad=bad)))'''
 return json.loads(m.shell(h,'python3 -c '+shlex.quote(code),timeout=45))
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:rows=list(pool.map(one,m.CLIENTS))
print(json.dumps(rows))
