#!/usr/bin/env python3
"""W78 real wrapper startup must retain unrelated processes behind stale records."""
import hashlib,json,os,shutil,socket,subprocess,tempfile,time
from pathlib import Path
source=Path(__file__).resolve().parents[2]
# Scratch under TMPDIR: removed on success, kept and printed on failure (K25).
work=Path(tempfile.mkdtemp(prefix='efs-wrapper-start-'));passed=False
unrelated=subprocess.Popen(['sleep','180']);dead=subprocess.Popen(['true']);dead.wait(5)
rows=[]
try:
    for label,value in [('invalid','not-a-pid'),('unrelated',str(unrelated.pid)),('dead',str(dead.pid))]:
        root=work/label;(root/'log').mkdir(parents=True);pidfile=root/'log/efsd.pid';pidfile.write_text(value)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
        start=['bash',str(source/'scripts/server.sh'),f'127.0.0.1:{port}',str(root),'--node-id','1','--direct-io']
        try:
            with (work/(label+'-start.log')).open('w') as out:
                subprocess.run(start,cwd=source,stdout=out,stderr=subprocess.STDOUT,check=True,timeout=15)
            pid=int(pidfile.read_text());assert pid!=unrelated.pid and unrelated.poll() is None
            with socket.create_connection(('127.0.0.1',port),2):pass
            stamp=json.loads(Path(str(pidfile)+'.identity').read_text())
            assert Path(stamp['exe']).resolve()==(source/'efsd').resolve()
            rows.append(dict(case=label,daemon_pid=pid,unrelated_pid=unrelated.pid,stamp=stamp,port=port))
        finally:
            with (work/(label+'-stop.log')).open('w') as out:
                subprocess.run(['bash',str(source/'scripts/server.sh'),'stop',str(root)],cwd=source,stdout=out,stderr=subprocess.STDOUT,check=True,timeout=90)
        assert unrelated.poll() is None and not pidfile.exists()
        assert not (Path('/proc')/str(pid)).exists() or (Path('/proc')/str(pid)/'stat').read_text().rsplit(')',1)[1].split()[0]=='Z'
        print('wrapper real start/stop:',label,'unrelated process retained PASS',flush=True)
    (work/'result.json').write_text(json.dumps(dict(cases=rows,binary_sha256=hashlib.sha256((source/'efsd').read_bytes()).hexdigest(),passed=True),indent=2))
    passed=True
finally:
    if unrelated.poll() is None:unrelated.terminate();unrelated.wait(5)
    if passed:shutil.rmtree(work,ignore_errors=True)
    else:print('Evidence kept:',work,flush=True)
