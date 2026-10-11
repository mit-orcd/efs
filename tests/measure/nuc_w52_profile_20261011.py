#!/usr/bin/env python3
"""One bounded, explicitly instrumented window during the exact long append gate."""
import hashlib,json,os,subprocess,time,traceback
from pathlib import Path

base=Path('/data1/efs/devel-nuc-20261011')
out=base/'w52-long-profile';out.mkdir(exist_ok=False)
end=time.monotonic()+14400
try:
    while True:
        assert time.monotonic()<end,'long append progress wait timeout'
        for phase in ('batch-build','batch-live','batch-install','w52-fixed-20000'):
            assert not (base/phase/'failure.txt').exists(),phase+' failed'
        progress=base/'w52-fixed-20000/append-burst-progress.json'
        if progress.exists():
            try:row=json.loads(progress.read_text())
            except json.JSONDecodeError:row={}
            if row.get('phase')=='writes' and row.get('records',0)>=2000:break
            assert row.get('phase')!='final-fsync','long gate finished before profile'
        time.sleep(1)
    images=json.loads((base/'batch-install/images.json').read_text())
    processes=[]
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:
            executable=os.readlink(proc/'exe')
            kind=Path(executable).name
            if executable not in ('/data1/efs/src/efsd','/data1/efs/src/efs-fuse'):continue
            digest=hashlib.sha256((proc/'exe').read_bytes()).hexdigest()
            assert digest==images[kind]['sha256'],(proc.name,kind,'image mismatch')
            processes.append(dict(pid=int(proc.name),kind=kind,sha256=digest))
        except (FileNotFoundError,ProcessLookupError,PermissionError):continue
    assert sum(p['kind']=='efsd' for p in processes)==4
    assert sum(p['kind']=='efs-fuse' for p in processes)==2
    before=dict(utc=time.strftime('%FT%TZ',time.gmtime()),monotonic=time.monotonic(),progress=row,
                processes=processes,diskstats=Path('/proc/diskstats').read_text())
    (out/'before.json').write_text(json.dumps(before,indent=2))
    ids=','.join(str(p['pid']) for p in processes)
    with (out/'perf-record.log').open('w') as log:
        rc=subprocess.run(['perf','record','-F','99','-g','--call-graph','fp','-p',ids,
            '-o',str(out/'perf.data'),'--','sleep','30'],stdout=log,stderr=subprocess.STDOUT,timeout=45).returncode
    (out/'perf-record-exit.txt').write_text(str(rc)+'\n');assert rc==0
    with (out/'perf-report.txt').open('w') as report:
        subprocess.run(['perf','report','--stdio','--percent-limit','0.5','-i',str(out/'perf.data')],
            stdout=report,stderr=subprocess.STDOUT,check=True,timeout=60)
    args=['timeout','--signal=INT','10s','strace','-f','-c','-w']
    for p in processes:
        if p['kind']=='efsd':args+=['-p',str(p['pid'])]
    with (out/'strace.txt').open('w') as trace:
        rc=subprocess.run(args,stdout=trace,stderr=subprocess.STDOUT,timeout=30).returncode
    (out/'strace-exit.txt').write_text(str(rc)+'\n');assert rc==124,'expected normal timeout SIGINT detach'
    after=dict(utc=time.strftime('%FT%TZ',time.gmtime()),monotonic=time.monotonic(),
               progress=json.loads(progress.read_text()),diskstats=Path('/proc/diskstats').read_text(),
               limits='Instrumented window; do not use as an unprofiled speed comparison. This does not establish a hardware ceiling.')
    (out/'after.json').write_text(json.dumps(after,indent=2))
    (out/'run-exit.txt').write_text('0\n')
except BaseException:
    (out/'failure.txt').write_text(traceback.format_exc());raise
