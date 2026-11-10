#!/usr/bin/env python3
# Task FB: run two gc_reclamation.py versions' files() against a concurrent export-directory churn
# for 5 s each and count FileNotFoundError. Usage: gc_files_race.py OLD_SOURCE NEW_SOURCE
import ast,os,shutil,sys,tempfile,threading,time
from pathlib import Path
def load(path):
    tree=ast.parse(Path(path).read_text())
    fns=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in ('export_entries','files')]
    return fns
tmp=Path(tempfile.mkdtemp(prefix='efs-taskFB-race-'))
folder=tmp/'data/exports/1/0000/0000/0000/0001/2345'
stop=False
def churn():
    while not stop:
        for s in 'abcdefgh':
            d=folder/s;d.mkdir(parents=True,exist_ok=True);(d/'frag').write_text('x')
        shutil.rmtree(folder,ignore_errors=True)
t=threading.Thread(target=churn);t.start()
for name in sys.argv[1:]:
    ns={'os':os,'Path':Path,'data_roots':[tmp]}
    exec(compile(ast.Module(body=load(name),type_ignores=[]),name,'exec'),ns)
    errors=0;end=time.time()+5;n=0
    while time.time()<end:
        n+=1
        try:ns['files'](12345)
        except FileNotFoundError:errors+=1
    print(f'{name}: scans={n} FileNotFoundError={errors}',flush=True)
stop=True;t.join();shutil.rmtree(tmp)
