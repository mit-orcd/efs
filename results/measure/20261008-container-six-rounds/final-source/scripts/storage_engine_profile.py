#!/usr/bin/env python3
"""Root-only profiler for a verified private benchmark daemon; never signals it."""
import argparse,json,os,signal,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('action',choices=['start','stop'])
p.add_argument('--case',type=Path,required=True)
p.add_argument('--phase',choices=['create','read','churn','delete'],required=True)
p.add_argument('--kind',choices=['strace','perf'],required=True)
a=p.parse_args();root=a.case
assert str(root).startswith('/data1/efs/bench/') and '..' not in root.parts
prefix=root/('profile-'+a.phase);control=Path(str(prefix)+'.json')
if a.action=='start':
    target=int((root/'pid').read_text());cmd=Path(f'/proc/{target}/cmdline').read_bytes().split(b'\0')
    assert str(root/'store').encode() in cmd and b'/tmp/efs-engine-ab-bin/efsd' in cmd
    assert not control.exists()
    stderr=open(str(prefix)+'.stderr','w')
    if a.kind=='strace':
        commands=[['strace','-f','-C','-w','-tt','-T','-yy','-qq','-e','trace=%file,fsync,fdatasync,pread64,pwrite64,fallocate,close,mmap,munmap,futex','-o',str(prefix)+'.strace','-p',str(target)]]
    else:
        commands=[['perf','record','-e','cycles','-F','199','--call-graph','fp','-p',str(target),'-o',str(prefix)+'.data'],
                  ['perf','stat','-e','cycles,instructions,task-clock,context-switches,page-faults','-p',str(target),'-o',str(prefix)+'.stat']]
    children=[subprocess.Popen(c,stdin=subprocess.DEVNULL,stdout=stderr,stderr=stderr,start_new_session=True).pid for c in commands]
    time.sleep(.5)
    for pid in children:assert Path(f'/proc/{pid}/cmdline').read_bytes(),'profiler exited during startup'
    control.write_text(json.dumps({'pids':children,'commands':commands,'target':target})+'\n')
else:
    state=json.loads(control.read_text())
    for pid in state['pids']:
        cmd=Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
        assert str(state['target']).encode() in cmd and any(str(prefix).encode() in v for v in cmd)
        os.kill(pid,signal.SIGINT)
    deadline=time.monotonic()+30
    for pid in state['pids']:
        f=Path(f'/proc/{pid}/cmdline')
        while f.exists() and f.read_bytes():
            if time.monotonic()>deadline:raise RuntimeError('profiler did not finalize')
            time.sleep(.1)
    if a.kind=='perf':
        for suffix,args in [('report',['--no-children','-F','overhead,comm,dso,symbol']),('calls',['--children','--call-graph','graph,0.5'])]:
            with open(str(prefix)+'.'+suffix,'w') as f:subprocess.run(['perf','report','--stdio','--percent-limit','0.5','-i',str(prefix)+'.data',*args],check=True,stdout=f,stderr=subprocess.STDOUT)
    control.with_suffix('.done').write_text('PASS\n')
