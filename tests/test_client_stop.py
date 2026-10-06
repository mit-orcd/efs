#!/usr/bin/env python3
"""Execute the actual client stop script with isolated command mocks."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='efs-client-stop-') as directory:
    task=Path(directory);(task/'scripts').mkdir();(task/'bin').mkdir()
    (task/'scripts/client.sh').write_text((root/'scripts/client.sh').read_text())
    (task/'scripts/client_processes.py').write_text("import os,sys\nprint('PROCESS:'+sys.argv[1],file=open(os.environ['STOP_LOG'],'a'))\nsys.exit(int(os.environ.get('RETIRE_RC','0')) if sys.argv[1]=='retire' else int(os.environ.get('PROCESS_RC','1')))\n")
    scripts={
      'efs-fuse':'''#!/bin/bash
echo "CONTROL:$*" >> "$STOP_LOG"
[ "$1" = --resume ] && exit 0
exit "${CONTROL_RC:-0}"
''',
      'mountpoint':'''#!/bin/bash
[ "$(cat "$STOP_STATE")" = 1 ]
''',
      'fusermount3':'''#!/bin/bash
echo "UNMOUNT:$*" >> "$STOP_LOG"
[ "${FAIL_UNMOUNT:-0}" = 1 ] && exit 1
[ "${FAIL_NORMAL:-0}" = 1 ] && [ "$1" = -u ] && exit 1
echo 0 > "$STOP_STATE"
''',
      'umount':'''#!/bin/bash
echo "UMOUNT:$*" >> "$STOP_LOG"
[ "${FAIL_UNMOUNT:-0}" = 1 ] && exit 1
[ "${FAIL_NORMAL:-0}" = 1 ] && [ "$1" != -l ] && exit 1
echo 0 > "$STOP_STATE"
''',
    }
    for name,text in scripts.items():
        p=task/name if name=='efs-fuse' else task/'bin'/name
        p.write_text(text);p.chmod(0o755)
    def run(force=False,**settings):
        state=task/'state';log=task/'log';state.write_text(str(settings.pop('MOUNTED',1))+'\n');log.write_text('')
        env=dict(os.environ,PATH=str(task/'bin')+':'+os.environ['PATH'],STOP_STATE=str(state),STOP_LOG=str(log),**{k:str(v) for k,v in settings.items()})
        r=subprocess.run(['bash',str(task/'scripts/client.sh'),'stop']+(['--force-discard'] if force else [])+['/efs-test-mount'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        return r.returncode,log.read_text(),state.read_text().strip()
    rc,log,state=run();assert rc==0 and state=='0' and 'CONTROL:--stop /efs-test-mount\n' in log
    for failure in (1,3):
        rc,log,state=run(CONTROL_RC=failure)
        assert rc!=0 and state=='1' and 'UNMOUNT:' not in log and 'UMOUNT:' not in log
    rc,log,state=run(FAIL_UNMOUNT=1)
    assert rc!=0 and state=='1' and '-uz' not in log and 'UMOUNT:-l' not in log and 'CONTROL:--resume' in log
    rc,log,state=run(True,CONTROL_RC=1)
    assert rc!=0 and state=='1' and 'UNMOUNT:' not in log
    rc,log,state=run(True,CONTROL_RC=3,FAIL_NORMAL=1)
    assert rc==0 and state=='0' and '--force-discard' in log and 'UNMOUNT:-uz' in log
    rc,log,state=run(MOUNTED=0,PROCESS_RC=0)
    assert rc==0 and 'CONTROL:--stop' in log and 'PROCESS:retire' in log
    rc,log,state=run(MOUNTED=0,PROCESS_RC=0,CONTROL_RC=1)
    assert rc!=0 and 'PROCESS:retire' not in log and 'UNMOUNT:' not in log
    rc,log,state=run(PROCESS_RC=2)
    assert rc!=0 and 'CONTROL:--stop' not in log and 'UNMOUNT:' not in log
    rc,log,state=run(MOUNTED=0,PROCESS_RC=0,RETIRE_RC=2)
    assert rc!=0 and 'PROCESS:retire' in log
    print('client stop: preflight refusal, no implicit lazy detach, resume and explicit force PASS')
