#!/usr/bin/env python3
"""Start/stop an owned two-host W100 fixture; never touch installed EFS stores.

Run locally with Intel SSH aliases. Source must already be built on both hosts.
Example: python3 scripts/intel_w100_private.py start baseline baseline
"""
import argparse
import json
import shlex
import subprocess
import time

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('action', choices=['start', 'stop'])
ap.add_argument('run')
ap.add_argument('source')
a = ap.parse_args()
for label in (a.run, a.source):
    if not label or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in label):
        ap.error('labels must be lowercase letters, digits or hyphens')
base = '/data1/efs/w100-private'
src = base + '/' + a.source
runtime = base + '/runtime-' + a.run
hosts = [('intel1_efs', '10.0.0.207', [1, 2]), ('intel2_efs', '10.0.0.209', [3, 4])]
env = dict(EFS_TRANSPORT='tcp', EFS_COMMANDS='0', EFS_COMMANDS_SERVER='0',
           EFS_PACKED_WRITES='0', EFS_EXPIRY='0', EFS_MD_RAFT_N='4',
           EFS_FAULT_REPORT_SIZE_FILE=runtime+'/omit-size',
           EFS_FAULT_REPORT_FILE=runtime+'/other-faults')


def remote(host, code):
    subprocess.run(['ssh', host, 'python3 -'], input=code, text=True, check=True)


def command(host, args):
    return subprocess.run(['ssh', host, shlex.join(args)], check=True)


if a.action == 'stop':
    for host, _, _ in hosts:
        remote(host, f"from pathlib import Path; Path({runtime+'/omit-size'!r}).write_text('OFF\\n')")
        command(host, [src+'/scripts/client.sh', 'stop', runtime+'/mnt'])
    for host, _, ids in hosts:
        for node in ids:
            storage = f'/data{1 if node%2 else 2}/efs/w100-private-{a.run}/n{node}'
            command(host, ['python3', src+'/scripts/server_processes.py', 'stop-file',
                           runtime+f'/n{node}.pid', storage])
    raise SystemExit(0)

for host, addr, ids in hosts:
    remote(host, f'''
import os,pathlib,socket,subprocess,json
r=pathlib.Path({runtime!r});r.mkdir(parents=True,exist_ok=False)
for port in (27432,27433):
    s=socket.socket();s.bind(({addr!r},port));s.close()
(r/'omit-size').write_text('OFF\\n');(r/'other-faults').write_text('OFF\\n')
env=os.environ.copy();env.update({env!r})
for node in {ids!r}:
    storage=f'/data{{1 if node%2 else 2}}/efs/w100-private-{a.run}/n{{node}}'
    pathlib.Path(storage).mkdir(parents=True,exist_ok=False)
    args=[{src+'/efsd'!r},'--node-id',str(node),'--addr',{addr!r},'--port',str(27432+(node%2==0)),
          '--storage',storage,'--quota','2G','--storage-engine','files','--direct-io']
    if node!=1:args+=['--join','10.0.0.207:27432']
    with (r/f'n{{node}}.log').open('wb') as out:
        p=subprocess.Popen(args,env=env,stdout=out,stderr=subprocess.STDOUT,start_new_session=True)
    (r/f'n{{node}}.pid').write_text(str(p.pid)+'\\n')
    print(json.dumps(dict(node=node,pid=p.pid,args=args)),flush=True)
''')

for attempt in range(45):
    r = subprocess.run(['ssh', hosts[0][0], shlex.join([src+'/efs-mgmt', 'raft-mkfs', '10.0.0.207:27432'])],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode == 0:
        print(r.stdout, flush=True)
        break
    time.sleep(1)
else:
    raise RuntimeError('private mkfs failed; preserve logs and stop owned fixture')
command(hosts[0][0], [src+'/efs-mgmt', 'commands-policy', '10.0.0.207:27432', 'set', '0', '0'])
for host, _, _ in hosts:
    seed = '10.0.0.209:27432' if host == 'intel1_efs' else '10.0.0.207:27432'
    prefix = ['env']+[k+'='+v for k,v in env.items()]
    command(host, prefix+[src+'/scripts/client.sh', seed, runtime+'/mnt'])
print(json.dumps(dict(runtime=runtime, source=src, features=env)), flush=True)
