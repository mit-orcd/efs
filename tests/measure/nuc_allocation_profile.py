#!/usr/bin/env python3
"""Separate instrumented allocation controls, after the uninstrumented ABBA run."""
import json,os,signal,socket,subprocess,time
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261010');src=base/'src';out=base/'allocation-profile'
out.mkdir(exist_ok=False)
def wait(fn,seconds,label):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if fn():return
        time.sleep(.5)
    raise RuntimeError(label+' timeout')
def run(args,path,timeout=120):
    with path.open('w') as log:
        result=subprocess.run(list(map(str,args)),stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
    path.with_suffix(path.suffix+'.exit').write_text(str(result.returncode)+'\n')
    return result.returncode
def listening(port):
    try:
        with socket.create_connection(('127.0.0.1',port),.3):return True
    except OSError:return False
stopped=False;bench=None
try:
    wait(lambda:(base/'closure/restore-exit.txt').exists(),6500,'ABBA completion and restoration')
    assert (base/'closure/run-exit.txt').read_text().strip()=='0'
    assert (base/'closure/restore-exit.txt').read_text().strip()=='0'
    pids=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split()
    assert len(pids)==4
    stopped=True
    for mount in ('mnt','mnt2'):
        assert run(['/data1/efs/src/scripts/client.sh','stop','/data1/efs/'+mount],out/(mount+'-stop.log'),180)==0
        assert not os.path.ismount('/data1/efs/'+mount)
    # Verify exact image and arguments before signaling the authorized service.
    for pid in pids:
        proc=Path('/proc',pid)
        assert os.path.realpath(proc/'exe')=='/data1/efs/src/efsd'
        assert b'/data1/efs/n' in (proc/'cmdline').read_bytes() or b'/data2/efs/n' in (proc/'cmdline').read_bytes()
        os.kill(int(pid),signal.SIGTERM)
    wait(lambda:all(not Path('/proc',p).exists() for p in pids),180,'regular daemon drain')
    assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1
    for drive,root in [('nvme',Path('/home/efs/devel-nuc-w96-20261010')),('sata',base/'w96-data')]:
        for mode in ('grow','ring'):
            cell=out/(drive+'-'+mode);cell.mkdir()
            log=(cell/'run.log').open('w')
            bench=subprocess.Popen(['python3',str(src/'tests/measure/nuc_durable_io.py'),str(root/('profile-'+drive+'-'+mode)),str(cell/'data'),'--mode',mode,'--seconds','180'],stdout=log,stderr=subprocess.STDOUT)
            # First writing a fallocated extent still needs conversion. Warm
            # through that pass before attributing ring profiles to overwrites.
            time.sleep(45);time.sleep(45);assert bench.poll() is None
            if mode=='ring':
                for writer in range(3):
                    windows=(cell/'data'/f'writer{writer}.jsonl').read_text().splitlines()
                    assert json.loads(windows[-1])['count']>=4096,'ring has not completed first extent-writing pass'
            proc=Path('/proc',str(bench.pid))
            assert str(src/'tests/measure/nuc_durable_io.py').encode() in (proc/'cmdline').read_bytes()
            tids=','.join(p.name for p in (proc/'task').iterdir())
            (cell/'identity.json').write_text(json.dumps(dict(pid=bench.pid,tids=tids,cmdline=(proc/'cmdline').read_bytes().decode().split('\0')),indent=2))
            events='xfs:xfs_log_force,xfs:xlog_iclog_force,xfs:xlog_iclog_sync,syscalls:sys_enter_fdatasync,syscalls:sys_enter_pwrite64,syscalls:sys_enter_ftruncate'
            run(['perf','stat','-x',',','-e',events,'-t',tids,'--','sleep','20'],cell/'tracepoints.txt',40)
            run(['perf','record','-F','99','-g','-p',str(bench.pid),'-o',cell/'cycles.data','--','sleep','15'],cell/'cycles-record.txt',35)
            run(['perf','report','--stdio','--no-children','--percent-limit','1','-i',cell/'cycles.data'],cell/'cycles-report.txt',40)
            run(['timeout','--signal=INT','10','strace','-f','-c','-p',str(bench.pid)],cell/'strace-summary.txt',30)
            rc=bench.wait(timeout=120);bench=None;log.close()
            (cell/'exit.txt').write_text(str(rc)+'\n');assert rc==0
    (out/'run-exit.txt').write_text('0\n')
except BaseException:
    import traceback
    (out/'failure.txt').write_text(traceback.format_exc());raise
finally:
    # Private file writers have a finite deadline; never abandon them in restore.
    if bench is not None:bench.wait(timeout=120)
    if stopped:
        assert not any(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
        env=dict(os.environ,EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_TRANSPORT='tcp',EFS_CLUSTER='nuc',EFS_DIRECT_IO='1')
        with (out/'restore.log').open('w') as log:
            rc=subprocess.run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300).returncode
        assert rc==0
        wait(lambda:all(listening(p) for p in range(17432,17436)),180,'regular service restored')
        assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
        (out/'restore-exit.txt').write_text('0\n')
