#!/usr/bin/env python3
"""Bounded CPU and path-resolved sync trace during the private direct 2k gate."""
import hashlib,json,os,re,subprocess,time,traceback
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261011');out=base/'clock-direct-profile';out.mkdir(exist_ok=False)
src=base/'clock-src';deadline=time.monotonic()+3600
try:
    while True:
        assert time.monotonic()<deadline,'private append phase wait exceeded'
        assert not (base/'clock-live/failure.txt').exists(),'live prerequisite failed'
        assert not (base/'clock-build/failure.txt').exists(),'native prerequisite failed'
        log=base/'clock-live/full-direct.log'
        if log.exists():
            match=re.search(r'^Evidence directory: (.+)$',log.read_text(),re.M)
            if match:
                work=Path(match[1]);assert work.parent==Path('/data1/efs') and work.name.startswith('gc-direct-')
                progress=work/'append-burst-progress.json'
                if progress.exists():
                    try:row=json.loads(progress.read_text())
                    except json.JSONDecodeError:row={}
                    if row.get('phase')=='writes' and 1200<=row.get('records',0)<1900:break
                    if row.get('phase')=='final-fsync' or row.get('records',0)>=1900:
                        (out/'not-measured.json').write_text(json.dumps(dict(reason='append phase finished/passed the selected late window before capture',progress=row)));raise SystemExit(0)
        if (base/'clock-live/full-direct-exit.txt').exists():
            (out/'not-measured.json').write_text(json.dumps(dict(reason='direct gate ended before the selected capture window')));raise SystemExit(0)
        time.sleep(1)
    images=json.loads((base/'clock-build/images.json').read_text());processes=[]
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:
            executable=os.readlink(proc/'exe');kind=Path(executable).name
            if executable not in (str(src/'efsd'),str(src/'efs-fuse')):continue
            digest=hashlib.sha256((proc/'exe').read_bytes()).hexdigest();assert digest==images[kind]['sha256']
            processes.append(dict(pid=int(proc.name),kind=kind,sha256=digest,command=(proc/'cmdline').read_bytes().replace(b'\0',b' ').decode()))
        except (FileNotFoundError,ProcessLookupError,PermissionError):continue
    assert sum(p['kind']=='efsd' for p in processes)==4 and sum(p['kind']=='efs-fuse' for p in processes)==2
    def snapshot():
        return dict(utc=time.strftime('%FT%TZ',time.gmtime()),monotonic=time.monotonic(),progress=json.loads(progress.read_text()),diskstats=Path('/proc/diskstats').read_text(),raft={str(p):subprocess.check_output([str(src/'efs-mgmt'),'raft-status',f'127.0.0.1:{p}'],text=True,timeout=15) for p in range(26590,26594)})
    (out/'before.json').write_text(json.dumps(dict(snapshot=snapshot(),processes=processes,work=str(work),limits='Instrumented private append window; not matched throughput or hardware-ceiling evidence. Raw path-resolved syscall durations include ptrace overhead.'),indent=2))
    with (out/'perf.log').open('w') as log:
        rc=subprocess.run(['perf','record','-F','99','-g','--call-graph','fp','-p',','.join(str(p['pid']) for p in processes),'-o',str(out/'perf.data'),'--','sleep','30'],stdout=log,stderr=subprocess.STDOUT,timeout=45).returncode
    (out/'perf-exit.txt').write_text(str(rc)+'\n');assert rc==0
    with (out/'flat.txt').open('w') as log:subprocess.run(['perf','report','--stdio','--no-children','--percent-limit','0.5','-i',str(out/'perf.data')],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
    command=['timeout','--signal=INT','10s','strace','-f','-yy','-ttt','-T','-e','trace=fsync,fdatasync','-o',str(out/'sync-paths.trace')]
    for p in processes:
        if p['kind']=='efsd':command+=['-p',str(p['pid'])]
    with (out/'strace.log').open('w') as log:rc=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=30).returncode
    (out/'strace-exit.txt').write_text(str(rc)+'\n');assert rc==124
    assert all(Path('/proc',str(p['pid'])).exists() for p in processes),'fixture restarted during capture; retain diagnostic, not a stable window'
    (out/'after.json').write_text(json.dumps(snapshot(),indent=2))
    (out/'run-exit.txt').write_text('0\n')
except SystemExit:raise
except BaseException:
    (out/'failure.txt').write_text(traceback.format_exc());raise
