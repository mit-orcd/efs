#!/usr/bin/env python3
"""Read-only ORCD ordered-frame queue sample; gdb reads types, never attaches.

Requires same-user /proc/PID/mem access and existing SSH access through the login.
Raw JSON goes to --output outside Git. Samples are racy observations, not credits.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--node', choices=['fcstor%03d' % i for i in range(3, 7)], required=True)
p.add_argument('--revision', required=True)
p.add_argument('--seconds', type=int, default=10)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--ssh-control')
a = p.parse_args()
assert re.fullmatch('[0-9a-f]{40}', a.revision)
assert 1 <= a.seconds <= 60

def remote(body):
    inner = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8',
             'erbmi1@' + a.node, 'bash -c ' + shlex.quote('set -euo pipefail\n' + body)]
    outer = ['ssh', '-A']
    if a.ssh_control:
        outer += ['-o', 'ControlPath=' + a.ssh_control]
    outer += ['erbmi1@orcd-login.mit.edu', shlex.join(inner)]
    return subprocess.check_output(outer, text=True, timeout=a.seconds + 60)

expr = ['p/x &g_host', 'p/x &((struct efs_raft_host*)0)->tx',
        'p/x sizeof(struct host_outbox)', 'p/x &((struct host_outbox*)0)->pending_entries',
        'p/x &((struct host_outbox*)0)->npri', 'p/x &((struct host_outbox*)0)->nent']
command = ['gdb', '-nx', '-batch', '/tmp/efs/efsd']
for e in expr:
    command += ['-ex', e]
types = remote(shlex.join(command))
values = [int(x, 16) for x in re.findall(r'\$\d+ = (0x[0-9a-f]+)', types)]
assert len(values) == 6, types
code = r"""import pathlib,os,struct,time,json
pid=int(pathlib.Path(PIDFILE).read_text());p=pathlib.Path('/proc')/str(pid)
assert p.stat().st_uid==os.getuid() and os.readlink(p/'exe')=='/tmp/efs/efsd'
assert pathlib.Path('/tmp/efs/.orcd-test-revision').read_text().strip()==REVISION
start=(p/'stat').read_text().rsplit(')',1)[1].split()[19]
typ=struct.unpack('<H',(p/'exe').read_bytes()[16:18])[0];slide=0
if typ==3:
 maps=[l.split() for l in (p/'maps').read_text().splitlines() if '/tmp/efs/efsd' in l]
 slide=next(int(m[0].split('-')[0],16) for m in maps if int(m[2],16)==0)
else:assert typ==2,typ
addr,tx,stride,pending,pri,ent=OFFSETS
fd=os.open(p/'mem',os.O_RDONLY);rows=[]
try:
 host=struct.unpack('Q',os.pread(fd,8,slide+addr))[0];assert host
 for i in range(SECONDS*20):
  slots=[]
  for peer in range(4):
   at=host+tx+stride*peer
   slots.append(dict(peer=peer+1,pending=struct.unpack('II',os.pread(fd,8,at+pending)),
    priority=struct.unpack('i',os.pread(fd,4,at+pri))[0],entries=struct.unpack('i',os.pread(fd,4,at+ent))[0]))
  rows.append(dict(sample=i,peers=slots));time.sleep(.05)
 assert (p/'stat').read_text().rsplit(')',1)[1].split()[19]==start
finally:os.close(fd)
print(json.dumps(dict(pid=pid,revision=REVISION,samples=rows)))
""".replace('PIDFILE', repr('/home/erbmi1/efs-runs/orcd-test/' + a.node + '/efsd.pid')).replace('REVISION', repr(a.revision)).replace('OFFSETS', repr(values)).replace('SECONDS', str(a.seconds))
result = json.loads(remote('python3 -c ' + shlex.quote(code)))
result['node'] = a.node
a.output.write_text(json.dumps(result) + '\n')
print(a.node, 'max entries', max(p['entries'] for s in result['samples'] for p in s['peers']),
      'max pending', max(max(p['pending']) for s in result['samples'] for p in s['peers']))
