#!/usr/bin/env python3
"""Actual numeric diagnostic sender: quiet, capped, nonblocking and balanced descriptors."""
from pathlib import Path
import os, shlex, socket, subprocess, tempfile, threading, time
root = Path(__file__).resolve().parents[1]
code = r'''
#include "query_stage_trace.h"
#include "efs/common.h"
#include <assert.h>
static int descriptors(void) {int n=0;for(int fd=0;fd<1024;fd++)if(fcntl(fd,F_GETFD)>=0)n++;return n;}
int main(int argc,char **argv) {
 assert(argc==3);if(!strcmp(argv[1],"off"))unsetenv("EFS_QUERY_WORK_SOCKET");
 else assert(!setenv("EFS_QUERY_WORK_SOCKET",argv[1],1));
 struct efs_query_capture_work w={.pins=4,.pins_ok=4,.pairs=4,.changed=1,.fast_eligible=1,
    .capture_ms=40,.spacing_ms=240,.dirty=256,.build_ms=0,.checkpoint_ms=0,.total_ms=41,
    .capture_rc=EFS_ERR_BUSY,.policy_interval_ms=2000,.reason="transaction-cut"};
 int before=descriptors();for(int i=0;i<atoi(argv[2]);i++)efs_query_capture_work_trace(2,EFS_ERR_BUSY,&w);
 assert(descriptors()==before);puts("nonblocking trace calls complete, descriptors balanced");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-work-trace-') as temp:
    base=Path(temp); source=base/'test.c'; binary=base/'test'
    source.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',
        '-I'+str(root/'include'),'-I'+str(root/'src/common'),str(source),'-o',str(binary)],check=True)
    target=base/'socket'; observed=[]
    with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as receiver:
        receiver.bind(str(target));receiver.settimeout(0.05)
        quiet=subprocess.run([str(binary),'off','3'],capture_output=True,text=True,check=True)
        try: receiver.recv(512);raise AssertionError('disabled trace emitted')
        except socket.timeout: pass
        def drain():
            while not stopped.is_set():
                try:observed.append(receiver.recv(512).decode('ascii'))
                except socket.timeout:continue
        stopped=threading.Event();thread=threading.Thread(target=drain);thread.start()
        try:
            subprocess.run([str(binary),str(target),'1'],capture_output=True,check=True,timeout=2)
            deadline=time.monotonic()+1
            while not observed and time.monotonic()<deadline:time.sleep(0.001)
            row=dict(x.split('=',1) for x in observed[0].split()[1:])
            assert row==dict(node='2',pid=row['pid'],seq='1',rc='-13',capture_reason='transaction-cut',
                capture_ms='40',source_ms='0',restore_ms='0',changed='1',fast_eligible='1',spacing_ms='240',pins='4',pins_ok='4',pairs='4',
                dirty='256',build_ms='0',checkpoint_ms='0',total_ms='41',capture_rc='-13',policy_interval_ms='2000')
            observed.clear()
            subprocess.run([str(binary),str(target),'600'],capture_output=True,check=True,timeout=2)
            time.sleep(0.1)
            sequences=[int(dict(x.split('=',1) for x in row.split()[1:])['seq']) for row in observed]
            assert sequences and max(sequences)<=512 and len(set(sequences))==len(sequences)
        finally:
            stopped.set();thread.join(timeout=1);assert not thread.is_alive()
    # A deliberately undrained socket and absent socket cannot hold a sender.
    with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as full:
        full.bind(str(base/'full'))
        for target in [str(base/'full'),str(base/'missing')]:
            subprocess.run([str(binary),target,'600'],capture_output=True,check=True,timeout=2)
print('actual work trace default quiet, exact bounded numeric payload, 512 attempt cap, missing/full observer nonblocking and descriptors balanced PASS')
