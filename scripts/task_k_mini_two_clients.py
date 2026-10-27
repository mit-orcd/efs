#!/usr/bin/env python3
"""Unchanged real-host POSIX two-client suite via the Task K SSH dispatcher."""
import importlib.util
from pathlib import Path
import subprocess
import sys
here=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('mini',here/'orcd_mini_posix2.py')
# That wrapper is a top-level program, so use its suite directly instead.
spec=importlib.util.spec_from_file_location('posix2',here.parent/'tests/posix/posix_2client.py')
suite=importlib.util.module_from_spec(spec);spec.loader.exec_module(suite)
dispatcher=str(here/'task_k_mini_ssh.sh')
mount='/data1/efs/mnt'
script='/data1/efs/src/tests/posix/posix_2client.py'
parent='taskK-mini-two-clients'
subprocess.run([dispatcher,'node9901',f'python3 {script} --prepare {mount} --parent {parent}'],check=True,timeout=120)
suite.PARENT=parent
raise SystemExit(suite.run_remote('node9901','node9900',mount,dispatcher,script,sys.argv[1],None))
