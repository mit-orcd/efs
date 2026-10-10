#!/usr/bin/python3
import os,signal,subprocess,sys
host,command=sys.argv[1:]
command=command.replace('/private/tmp/efs-mac-completion-harness/tests/posix/posix_2client.py','/tmp/efs/tests/posix/posix_2client.py')
sec=int(os.environ.get('EFS_SSH_TIMEOUT','180'))
p=subprocess.Popen(['/bin/bash','/tmp/efs-two-host-command.sh',host,command],start_new_session=True)
try:sys.exit(p.wait(timeout=sec+10))
except subprocess.TimeoutExpired:
 os.killpg(p.pid,signal.SIGKILL);p.wait();sys.exit(124)
