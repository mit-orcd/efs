#!/usr/bin/env python3
"""Private KV barrier failure: saved applied must not pass durable KV.

Three unique stores, no service mounts or stores. Restore the faulted replica's
cold pre-fault KV image to model loss of its unsynced tail; retain its durable
Raft log and applied markers. This is injected EIO/process recovery, not a
physical power-loss certification.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mode',choices=('direct','buffered'),default='buffered' if os.environ.get('EFS_TEST_DIRECT_IO')=='0' else 'direct',help='storage I/O mode; default preserves the original direct fixture')
options=parser.parse_args()
source=Path(__file__).resolve().parents[2]
server=Path(os.environ.get('EFS_TEST_SERVER_BINARY',str(source/'efsd')))
work=Path(tempfile.mkdtemp(prefix='efs-checkpoint-fault-',dir=os.environ.get('EFS_TEST_WORK_ROOT','/data1/efs')))
(work/'manifest.json').write_text(json.dumps({'mode':options.mode,'transport':'tcp','source':str(source),'binary_sha256':{name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in [('efsd',server),('efs-mgmt',source/'efs-mgmt'),('libefs.a',source/'libefs.a')]}},indent=2))
print('Evidence:',work,flush=True)
base=22430
for port in range(base,base+3):
    with socket.socket() as sock:sock.bind(('127.0.0.1',port))
shim=r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int fdatasync(int fd) {
    static int (*real_sync)(int);
    if (!real_sync) real_sync=dlsym(RTLD_NEXT,"fdatasync");
    const char *flag=getenv("TEST_KV_BARRIER_FILE");
    if (flag && access(flag,F_OK)==0) {
        char link[64],path[PATH_MAX];
        snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
        ssize_t n=readlink(link,path,sizeof(path)-1);
        if (n>0) {
            path[n]=0;
            if (strstr(path,"/mdraft/kv/wal")) { errno=EIO; return -1; }
        }
    }
    return real_sync(fd);
}
'''
reader=r'''
#include <stdio.h>
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    struct efs_kv_lsm_cfg cfg={0};
    struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);
    if(!kv)return 3;
    struct efs_meta_dentry d;
    int rc=efs_meta_apply_lookup(kv,1,"after-kv-fault",&d);
    printf("rc=%d ino=%llu\n",rc,(unsigned long long)(rc?0:d.ino));
    efs_kv_lsm_close(kv);return rc?4:0;
}
'''
(work/'fault.c').write_text(shim);(work/'reader.c').write_text(reader)
subprocess.run(['cc','-Wall','-Wextra','-Werror','-shared','-fPIC',work/'fault.c','-ldl','-o',work/'fault.so'],check=True)
subprocess.run(['cc','-I'+str(source/'include'),work/'reader.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',work/'reader'],check=True)
env=dict(os.environ,EFS_TRANSPORT='tcp',EFS_COMMANDS_SERVER='0',
         EFS_RAFT_BATCH_NOOP='1')
servers=[];logs=[];generation=0
def mgmt(node,cmd,*args):
    return subprocess.check_output([str(source/'efs-mgmt'),cmd,f'127.0.0.1:{base+node}',*[str(x) for x in args]],text=True,timeout=10)
def wait(fn,label):
    end=time.monotonic()+60
    while time.monotonic()<end:
        value=fn()
        if value:return value
        time.sleep(.1)
    raise AssertionError(label)
def status(node):
    text=mgmt(node,'raft-status');rows={}
    for m in re.finditer(r'group (\d+) hosted=(\d+) role=(\w+) leader=(-?\d+) term=(\d+) commit=(\d+) applied=(\d+)',text):
        if m[2]=='1':rows[int(m[1])]=dict(role=m[3],commit=int(m[6]),applied=int(m[7]))
    return rows
def leader():
    for node in range(3):
        try:
            if status(node).get(0,{}).get('role','').lower()=='leader':return node+1
        except subprocess.SubprocessError:pass
    return None
def start():
    global generation
    generation+=1
    for node in range(3):
        root=work/f'n{node}';root.mkdir(exist_ok=True)
        log=open(work/f'n{node}-{generation}.log','w');logs.append(log)
        child=dict(env,LD_PRELOAD=str(work/'fault.so'),TEST_KV_BARRIER_FILE=str(work/f'fail-{node}'))
        cmd=[str(server),'--node-id',str(node+1),'--addr','127.0.0.1','--port',str(base+node),'--storage',str(root),
             '--direct-io' if options.mode=='direct' else '--no-direct-io']
        if node:cmd+=['--join',f'127.0.0.1:{base}']
        proc=subprocess.Popen(cmd,env=child,stdout=log,stderr=log);servers.append(proc)
        def listening():
            assert proc.poll() is None,'server exited'
            try:
                with socket.create_connection(('127.0.0.1',base+node),.2):return True
            except OSError:return None
        wait(listening,'listen')
def stop():
    for proc in servers:
        if proc.poll() is None:proc.terminate()
    for proc in servers:
        assert proc.wait(timeout=60)==0,'nonzero graceful server exit'
    servers.clear()
    for f in logs:f.close()
    logs.clear()
def markers(node):
    result={}
    for group in (0,2):
        f=work/f'n{node}/mdraft/applied.{group}'
        if not f.exists():result[group]=0;continue
        raw=f.read_bytes();assert len(raw)==16
        index,check=struct.unpack('>QQ',raw);assert check==(index^((1<<64)-1))
        result[group]=index
    return result
try:
    start();old=wait(leader,'initial leader')-1
    wait(lambda:'rc=0' in mgmt(old,'raft-mkfs'),'format')
    assert 'status=0' in mgmt(old,'raft-create',1,'before-kv-fault')
    cut={g:r['commit'] for g,r in status(old).items()}
    wait(lambda:all(status(n)[g]['applied']>=idx for n in range(3) for g,idx in cut.items()),'baseline replicated')
    stop()
    # A closed cold image contains all pre-fault KV state/segments, not just WAL.
    for node in range(3):shutil.copytree(work/f'n{node}/mdraft/kv',work/f'baseline-kv-{node}')
    start();old=wait(leader,'restarted leader')-1;fault=(old+1)%3
    time.sleep(.3);before=markers(fault)
    (work/f'fail-{fault}').touch()
    created=mgmt(old,'raft-create',1,'after-kv-fault')
    assert 'status=0' in created,created
    ino=int(re.search(r'ino=(\d+)',created)[1])
    wait(lambda:'kv wal fsync failed at pump release' in (work/f'n{fault}-{generation}.log').read_text(),'injected KV release failure')
    time.sleep(.3);after=markers(fault)
    (work/'marker-observations.json').write_text(json.dumps(dict(before=before,after_failure=after),indent=2))
    assert after==before,(before,after)
    cut={g:r['commit'] for g,r in status(old).items()}
    stop();shutdown=markers(fault);assert shutdown==before,(before,shutdown)
    (work/f'fail-{fault}').unlink()
    # Only this private failed replica is modified: model unsynced KV loss.
    kv=work/f'n{fault}/mdraft/kv';kv.rename(work/'failed-kv-retained')
    shutil.copytree(work/f'baseline-kv-{fault}',kv)
    start();wait(leader,'recovery leader')
    wait(lambda:all(status(n)[g]['applied']>=idx for n in range(3) for g,idx in cut.items()),'durable Raft replay catches up')
    stop()
    cold=subprocess.check_output([str(work/'reader'),str(kv)],text=True)
    assert cold.strip()==f'rc=0 ino={ino}',cold
    record=dict(fault_node=fault+1,before=before,after_failure=after,after_shutdown=shutdown,
                acknowledged_inode=ino,leader_cut=cut,cold_lookup=cold.strip(),
                unsynced_kv_loss_modeled=True,build=subprocess.check_output([server,'--version'],text=True).strip())
    (work/'result.json').write_text(json.dumps(record,indent=2))
    print('PASS: KV EIO cannot advance pump/shutdown checkpoints; retained Raft replay restores cold acknowledged inode',flush=True)
finally:
    if servers:stop()
