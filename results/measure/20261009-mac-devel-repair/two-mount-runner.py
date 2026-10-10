#!/usr/bin/python3
import os, subprocess, sys
host, command = sys.argv[1:]
if host == 'mount-b':
    command = command.replace('/mnt/efs-e/', '/mnt/efs-f/')
sec = os.environ.get('EFS_SSH_TIMEOUT', '45')
sys.exit(subprocess.call(['timeout', '--kill-after=5s', sec + 's', 'bash', '-c', command]))
