#!/usr/bin/env python3
"""Private wrapper process fixtures: no operator processes or storage."""
from pathlib import Path
import importlib.util
import json
import os
import subprocess
import tempfile
import time
root=Path(__file__).resolve().parents[1]
helper=root/'scripts/server_processes.py'
spec=importlib.util.spec_from_file_location('processes',helper)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

# K1/R4: classification of an efsd stop from its exit record. Only an
# orderly exit with every durability barrier done is orderly.
def rec(**fields):
    base=dict(pid='4242',outcome='orderly',rc='0',phase='done',worker='none',elapsed_ms='12',
              deadline_ms='50000',put_sync='done',reclaim_journal='done',usage_flush='done',stores='closed')
    base.update(fields);return base
assert module.classify(4242,rec())[0]=='orderly'
assert 'stopped, orderly (rc 0' in module.classify(4242,rec())[1]
forced=module.classify(4242,rec(outcome='forced',rc='3',phase='packer',worker='packer thread',
                                put_sync='not_reached',reclaim_journal='not_reached',usage_flush='not_reached',
                                stores='preserved',elapsed_ms='50003'))
assert forced[0]=='forced' and 'stopped, forced, stores preserved' in forced[1]
assert 'phase packer, worker packer thread' in forced[1] and 'put_sync not_reached' in forced[1]
assert module.classify(4242,rec(outcome='barrier-failed',rc='4',put_sync='failed'))[0]=='barrier-failed'
for bad in (rec(usage_flush='failed'),rec(rc='3'),rec(outcome='forced'),rec(pid='4243'),rec(outcome='other'),None):
    outcome,message=module.classify(4242,bad)
    assert outcome!='orderly' and 'not an orderly stop' in message,(bad,outcome,message)
assert module.classify(4242,None)[0]=='unknown'
assert module.OUTCOME_RC=={'orderly':0,'forced':3,'barrier-failed':4,'unknown':4}
assert module.exit_record_path({'args':['efsd','--storage','/a,/b']})==Path('/a/log/efsd.exit')
with tempfile.TemporaryDirectory(prefix='efs-exit-record-') as work:
    path=Path(work)/'efsd.exit'
    assert module.read_exit_record(path) is None
    path.write_text('garbage\npid=1\n');assert module.read_exit_record(path) is None
    path.write_text('efsd-exit 1\npid=7\noutcome=forced\nworker=a=b\n')
    assert module.read_exit_record(path)=={'pid':'7','outcome':'forced','worker':'a=b'}
print('exit classification: orderly only with rc 0 and every barrier done; forced/barrier/unknown/stale refused PASS')

# A fake efsd: on SIGTERM, --exit-mode orderly|forced writes this process's
# exit record and exits 0|3; none dies of the signal without a record.
FAKE=r'''
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *storage,*mode="none";
static volatile sig_atomic_t stop;
static void done(int s){(void)s;stop=1;}
int main(int argc,char **argv){
 for(int i=1;i+1<argc;i++){if(!strcmp(argv[i],"--storage"))storage=argv[i+1];if(!strcmp(argv[i],"--exit-mode"))mode=argv[i+1];}
 signal(SIGINT,SIG_DFL);
 if(!strcmp(mode,"none")){signal(SIGTERM,SIG_DFL);for(;;)pause();}
 signal(SIGTERM,done);while(!stop)pause();
 int forced=!strcmp(mode,"forced");char path[4096];snprintf(path,sizeof path,"%s/log/efsd.exit",storage);
 FILE *f=fopen(path,"w");if(!f)return 9;
 fprintf(f,"efsd-exit 1\npid=%ld\noutcome=%s\nrc=%d\nphase=%s\nworker=%s\nelapsed_ms=50001\ndeadline_ms=50000\n"
   "put_sync=%s\nreclaim_journal=%s\nusage_flush=%s\nstores=%s\n",(long)getpid(),forced?"forced":"orderly",forced?3:0,
   forced?"reclaim":"done",forced?"reclaim journal, deleter and recovery threads":"none",
   "done",forced?"stuck":"done",forced?"not_reached":"done",forced?"preserved":"closed");
 fclose(f);return forced?3:0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-owner-') as work:
    directory=Path(work);storage=directory/'store';storage.mkdir();(storage/'log').mkdir()
    # nohup can give this Python runner an ignored SIGINT disposition. The
    # fake perf must model a recorder that handles SIGINT, not inherit ignore.
    source=directory/'process.c';source.write_text(FAKE)
    binary=directory/'efsd'
    subprocess.run(['cc',str(source),'-o',str(binary)],check=True)
    unrelated=subprocess.Popen(['sleep','120'])
    daemon=subprocess.Popen([str(binary),'--storage',str(storage),'--port','21950'])
    pidfile=storage/'log/efsd.pid'
    def wrapper():
        subprocess.run(['bash',str(root/'scripts/server.sh'),'stop',str(storage)],check=True,timeout=10)
    try:
        wrapper() # Absent record must leave both live processes untouched.
        assert unrelated.poll() is None and daemon.poll() is None
        dead=subprocess.Popen(['true']);dead.wait(5)
        pidfile.write_text(str(dead.pid));wrapper()
        assert unrelated.poll() is None and daemon.poll() is None
        for _ in range(100):
            if module.owns(module.identity(daemon.pid),'storage',str(storage)):break
            time.sleep(.01)
        for bad in ('-1','not-a-pid',str(unrelated.pid)):
            pidfile.write_text(bad);wrapper()
            assert unrelated.poll() is None and daemon.poll() is None
        pidfile.write_text(str(daemon.pid))
        subprocess.run(['python3',str(helper),'record',str(pidfile),str(storage)],check=True)
        stamp=Path(str(pidfile)+'.identity')
        info=json.loads(stamp.read_text());info['start']='wrong-start';stamp.write_text(json.dumps(info))
        wrapper();assert daemon.poll() is None,'stale start identity killed daemon'
        wrong=directory/'other';wrong.mkdir();(wrong/'log').mkdir()
        (wrong/'log/efsd.pid').write_text(str(daemon.pid))
        subprocess.run(['bash',str(root/'scripts/server.sh'),'stop',str(wrong)],check=True,timeout=10)
        assert daemon.poll() is None,'wrong storage killed daemon'
        # The matching daemon dies of SIGTERM without an exit record: it is
        # gone, but the stop is not orderly (rc 4), and a stale record left
        # by an earlier incarnation cannot speak for it.
        (storage/'log/efsd.exit').write_text(f'efsd-exit 1\npid={daemon.pid}\noutcome=orderly\nrc=0\n'
                                             'put_sync=done\nreclaim_journal=done\nusage_flush=done\n')
        pidfile.write_text(str(daemon.pid))
        subprocess.run(['python3',str(helper),'record',str(pidfile),str(storage)],check=True)
        out=subprocess.run(['bash',str(root/'scripts/server.sh'),'stop',str(storage)],capture_output=True,text=True,timeout=10)
        daemon.wait(5)
        assert out.returncode==4 and 'outcome unknown' in out.stdout and 'Stopped efsd' not in out.stdout,(out.returncode,out.stdout,out.stderr)
        assert daemon.returncode==-15 and not pidfile.exists() and unrelated.poll() is None
        sentinel=storage/'data/sentinel';(storage/'data').mkdir();sentinel.write_bytes(b'kept')
        def owned(mode):
            process=subprocess.Popen([str(binary),'--storage',str(storage),'--port','21950','--exit-mode',mode])
            pidfile.write_text(str(process.pid))
            for _ in range(100):
                if module.owns(module.identity(process.pid),'storage',str(storage)):break
                time.sleep(.01)
            subprocess.run(['python3',str(helper),'record',str(pidfile),str(storage)],check=True)
            return process
        # Orderly: rc 0 and reported as such.
        daemon=owned('orderly')
        out=subprocess.run(['bash',str(root/'scripts/server.sh'),'stop',str(storage)],capture_output=True,text=True,timeout=10)
        assert daemon.wait(5)==0 and out.returncode==0,(out.stdout,out.stderr)
        assert 'stopped, orderly (rc 0' in out.stdout and 'Stopped efsd for storage' in out.stdout
        # Forced: the process is gone, stores preserved, rc 3, never orderly.
        daemon=owned('forced')
        out=subprocess.run(['python3',str(helper),'stop-file',str(pidfile),str(storage)],capture_output=True,text=True,timeout=10)
        assert daemon.wait(5)==3 and out.returncode==3,(out.stdout,out.stderr)
        assert 'stopped, forced, stores preserved' in out.stdout and 'phase reclaim' in out.stdout
        assert 'reclaim_journal stuck' in out.stdout and not pidfile.exists()
        assert sentinel.read_bytes()==b'kept'
        daemon=owned('forced')
        out=subprocess.run(['bash',str(root/'scripts/server.sh'),'stop',str(storage)],capture_output=True,text=True,timeout=10)
        assert daemon.wait(5)==3 and out.returncode==3 and 'Stopped efsd' not in out.stdout,(out.stdout,out.stderr)
        assert 'without an orderly shutdown (rc 3); stores preserved' in out.stderr
        # A restart over a daemon that stops forced starts nothing and says so.
        daemon=owned('forced')
        out=subprocess.run(['bash',str(root/'scripts/server.sh'),'127.0.0.1:21951',str(storage)],capture_output=True,text=True,timeout=10)
        assert daemon.wait(5)==3 and out.returncode==3,(out.stdout,out.stderr)
        assert 'did not stop orderly (rc 3); stores preserved, nothing started' in out.stderr
        assert 'efsd started' not in out.stdout and not pidfile.exists() and sentinel.read_bytes()==b'kept'
        print('server wrapper: orderly rc 0; forced rc 3 "stopped, forced, stores preserved"; unknown rc 4; restart over a forced stop refused PASS')
        perf_binary=directory/'perf'
        subprocess.run(['cc',str(source),'-o',str(perf_binary)],check=True)
        output=directory/'efsd.data';perf_pid=directory/'perf.pid'
        recorder=subprocess.Popen([str(perf_binary),'record','-p',str(unrelated.pid),'-o',str(output)])
        try:
            perf_pid.write_text(str(recorder.pid))
            subprocess.run(['python3',str(helper),'record-perf',str(perf_pid),str(output)],check=True)
            subprocess.run(['python3',str(helper),'stop-perf',str(perf_pid),str(directory/'wrong.data')],check=True)
            assert recorder.poll() is None,'wrong output killed recorder'
            perf_pid.write_text(str(unrelated.pid))
            subprocess.run(['python3',str(helper),'stop-perf',str(perf_pid),str(output)],check=True)
            assert unrelated.poll() is None,'unrelated PID killed by perf cleanup'
            perf_pid.write_text(str(recorder.pid))
            subprocess.run(['python3',str(helper),'record-perf',str(perf_pid),str(output)],check=True)
            subprocess.run(['python3',str(helper),'stop-perf',str(perf_pid),str(output)],check=True)
            recorder.wait(5);assert recorder.returncode==-2
            print('perf wrapper: unrelated/wrong-output processes retained; owned recorder flushed with SIGINT PASS')
        finally:
            if recorder.poll() is None:recorder.terminate();recorder.wait(5)
        print('server wrapper: invalid/reused/wrong-storage/stale-start PIDs retained; matching daemon gracefully stopped PASS')
    finally:
        for proc in (unrelated,daemon):
            if proc.poll() is None:proc.terminate();proc.wait(5)
# Real launch coverage lives outside TemporaryDirectory so a failed retirement
# preserves its owned stores/logs rather than removing a live daemon's root.
# Quota admission may require more headroom than a small VM tmpfs provides.
# Scope the real daemon's storage fixture override to this subprocess; other
# tests keep their ordinary temporary-directory behavior.
fixture_env=dict(os.environ)
if os.environ.get('EFS_TEST_STORAGE_TMPDIR'):
    fixture_env['TMPDIR']=os.environ['EFS_TEST_STORAGE_TMPDIR']
subprocess.run(['python3',str(root/'tests/live/server_wrapper_start.py')],
               env=fixture_env,check=True,timeout=360)
