#!/usr/bin/env python3
"""Unchanged real-host POSIX two-client suite via the Task K SSH dispatcher."""
import importlib.util
from pathlib import Path
import subprocess
import os
import shlex
import sys
here=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('posix2',here.parent/'tests/posix/posix_2client.py')
suite=importlib.util.module_from_spec(spec);spec.loader.exec_module(suite)
dispatcher=str(here/'task_k_mini_ssh.sh')
mount='/data1/efs/mnt'
script='/data1/efs/src/tests/posix/posix_2client.py'
parent='taskK-mini-two-clients'
subprocess.run([dispatcher,'node9901',f'python3 {script} --prepare {mount} --parent {parent}'],check=True,timeout=120)
# Same 120-second per-step bound as scripts/orcd_mini_posix2.py.
def bounded(ssh,host,remote):
    sec=int(os.environ.get('POSIX2_STEP_SEC','120'))
    command='timeout --signal=TERM --kill-after=5s %ss sh -c %s' % (sec,shlex.quote(remote))
    try:
        result=subprocess.run([ssh,host,command],capture_output=True,text=True,timeout=sec+30)
        return result.returncode,result.stdout,result.stderr
    except subprocess.TimeoutExpired:
        return 124,'','step timed out after %s seconds' % sec
suite.ssh_cmd=bounded
suite.PARENT=parent
raise SystemExit(suite.run_remote('node9901','node9900',mount,dispatcher,script,sys.argv[1],None))
