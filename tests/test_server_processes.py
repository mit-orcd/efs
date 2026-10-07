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
with tempfile.TemporaryDirectory(prefix='efs-owner-') as work:
    directory=Path(work);storage=directory/'store';storage.mkdir();(storage/'log').mkdir()
    source=directory/'process.c';source.write_text('#include <unistd.h>\nint main(void){for(;;)pause();}\n')
    binary=directory/'efsd'
    subprocess.run(['cc',str(source),'-o',str(binary)],check=True)
    unrelated=subprocess.Popen(['sleep','120'])
    daemon=subprocess.Popen([str(binary),'--storage',str(storage),'--port','21950'])
    pidfile=storage/'log/efsd.pid'
    def wrapper():
        subprocess.run(['bash',str(root/'scripts/server.sh'),'stop',str(storage)],check=True,timeout=10)
    try:
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
        pidfile.write_text(str(daemon.pid))
        subprocess.run(['python3',str(helper),'record',str(pidfile),str(storage)],check=True)
        wrapper();daemon.wait(5)
        assert daemon.returncode==-15 and not pidfile.exists()
        assert unrelated.poll() is None
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
