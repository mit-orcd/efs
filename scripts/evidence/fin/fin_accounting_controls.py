#!/usr/bin/env python3
"""Run the real socket oracle against candidate, old and unsafe FIN accounting.

SRC OLD_RDMA_C FRESH_EVIDENCE. Export OLD_RDMA_C with git show b4d512ace:src/common/rdma.c.
Only fresh private loopback sockets; generated sources/output stay outside Git.
"""
import json
from pathlib import Path
import resource
import signal
import subprocess
import sys

src, old, art = (Path(x).resolve() for x in sys.argv[1:])
art.mkdir()
body = old.read_text()
a = body.index('static int tcp_counts(')
b = body.index('static void setup_publish', a)
original = body[a:b]
assert 'info.tcpi_state==8;' in original, 'expected pinned old accounting body'
prefix = '''#include <linux/tcp.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
'''
adapter = '''
static int efs_tcp_payload_counts(int fd,uint64_t *tx,uint64_t *rx,unsigned *state,unsigned *fin)
{
    int seen=0;int rc=tcp_counts(fd,tx,rx,&seen);
    if(rc)return rc;
    *state=0;*fin=(unsigned)seen;
    *rx=*rx>=*fin?*rx-*fin:0;
    return 0;
}
'''
# The saturation matches the old post_setup calculation, never invents an underflow.
headers = dict(candidate=src/'src/common/tcp_counts.h')
for name, code in (('old', original), ('unsafe_closed', original.replace(
        'info.tcpi_state==8;', '(info.tcpi_state==8 || info.tcpi_state==7);'))):
    p = art/(name+'.h')
    p.write_text(prefix+code+adapter)
    headers[name] = p
text = (src/'tests/test_tcp_counts.c').read_text()
results = {}

def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

for name, header in headers.items():
    test = art/(name+'.c')
    test.write_text(text.replace('"../src/common/tcp_counts.h"', '"'+str(header)+'"'))
    binary = art/name
    with (art/(name+'-build.log')).open('wb') as log:
        build = subprocess.run(['gcc','-D_GNU_SOURCE','-std=gnu11','-Wall','-Wextra','-Werror',
                                '-O1','-g',str(test),'-o',str(binary)],stdout=log,stderr=subprocess.STDOUT).returncode
    code = None
    if build == 0:
        with (art/(name+'.log')).open('wb') as log:
            code = subprocess.run([str(binary)],stdout=log,stderr=subprocess.STDOUT,
                                  timeout=30,preexec_fn=no_core).returncode
    expected = 0 if name == 'candidate' else -signal.SIGABRT
    trace = (art/(name+'.log')).read_text() if code is not None else ''
    checks = sum(line.startswith('rx=') and line.endswith(' PASS') for line in trace.splitlines())
    trace_ok = ('TCP payload accounting:' in trace if name == 'candidate' else
                'TCP accounting did not reach expected payload/FIN counts' in trace and
                checks == (4 if name == 'old' else 10))
    results[name] = dict(build=build, exit=code, expected=expected, positive_prefix_checks=checks,
                         verdict='PASS' if build==0 and code==expected and trace_ok else 'FAIL')
(art/'results.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results,indent=2))
raise SystemExit(any(x['verdict']!='PASS' for x in results.values()))
