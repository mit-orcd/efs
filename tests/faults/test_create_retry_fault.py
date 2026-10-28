#!/usr/bin/env python3
"""Exercise the actual CREATE fault hook for legacy and retained framing."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('EFS_CREATE_FAULT_SOURCE',
                            str(root / 'src/client/inode_rpc.c'))).read_text()
match = re.search(r'^static int rpc_fault_create_reply\([^;]+?\)\n\{',
                  source, re.M)
assert match
# Retained replies pass through the common transport hook. A second injection
# here doubles the lost/BUSY stages and invalidates the mounted three-stage gate.
retained = re.search(r'^static int create_request_rpc\([^;]+?\)\n\{', source, re.M)
assert retained
retained_body = source[retained.start():source.index('\n}', retained.end()) + 2]
assert 'EFS_FAULT_CREATE_RETRY_FILE' not in retained_body, 'duplicate retained CREATE fault injection'
assert 'test-create-retry:' not in retained_body, 'duplicate retained CREATE fault stage'
function = source[match.start():source.index('\n}', match.end()) + 2]
code = r'''
#include "efs/protocol.h"
#include "efs/opid.h"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
'''+function+r'''
static void exercise(unsigned retained, unsigned mismatch) {
    struct efs_msg_create_retained request={0};
    struct efs_msg_create_retained_reply reply={0};
    strcpy(request.create.name,"w58-live-retry");
    request.create.mode=0100600;
    request.q.id.client_uuid[0]=19;
    request.q.id.session_epoch=7; request.q.id.seq=42;
    reply.rpc.inode.ino=512; reply.rpc.status=EFS_INODE_RPC_OK;
    reply.known=reply.routed=1; reply.generation=9; reply.shard=512;
    unsigned char legacy[sizeof(request.create)+EFS_OPID_WIRE_LEN];
    memcpy(legacy,&request.create,sizeof(request.create));
    efs_opid_req_pack(&request.q,legacy+sizeof(request.create));
    unsigned type=retained?EFS_MSG_CREATE_RETAINED:EFS_MSG_INODE_CREATE;
    unsigned rt=retained?EFS_MSG_CREATE_RETAINED_REPLY:EFS_MSG_INODE_CREATE_REPLY;
    unsigned len=retained?sizeof(request):sizeof(legacy);
    unsigned plen=retained?sizeof(reply):sizeof(reply.rpc);
    void *req=retained?(void *)&request:(void *)legacy;
    if (retained) {
        reply.known=0;
        assert(rpc_fault_create_reply(type,req,len,rt,&reply,plen)==0);
        reply.known=1;
    }
    strcpy(request.create.name,"unrelated");
    if (!retained)memcpy(legacy,&request.create,sizeof(request.create));
    assert(rpc_fault_create_reply(type,req,len,rt,&reply,plen)==0);
    strcpy(request.create.name,"w58-live-retry");
    if (!retained)memcpy(legacy,&request.create,sizeof(request.create));
    assert(rpc_fault_create_reply(type,req,len,rt,&reply,plen-1)==0);
    assert(rpc_fault_create_reply(type,req,len,rt,&reply,plen)==-1);
    assert(reply.rpc.status==EFS_INODE_RPC_OK && reply.rpc.inode.ino==512);
    /* Probe/ACK may advance; neither changes the creation identity. */
    request.probe=1; request.q.ack=41;
    if (mismatch==1)request.q.id.seq++;
    if (mismatch==2)request.q.id.session_epoch++;
    if (mismatch==3)request.q.id.client_uuid[0]++;
    if (mismatch==4)request.create.uid++;
    if (mismatch==5)reply.rpc.inode.ino++;
    if (mismatch==6)reply.generation++;
    if (mismatch==7)reply.shard++;
    if (!retained) {
        memcpy(legacy,&request.create,sizeof(request.create));
        efs_opid_req_pack(&request.q,legacy+sizeof(request.create));
    }
    assert(rpc_fault_create_reply(type,req,len,rt,&reply,plen)==0);
    assert(reply.rpc.status==EFS_INODE_RPC_BUSY);
    if (retained)assert(!reply.known && !reply.generation && !reply.rpc.inode.ino);
    reply.rpc.status=EFS_INODE_RPC_OK;reply.rpc.inode.ino=512;
    reply.known=1;reply.generation=9;
    assert(rpc_fault_create_reply(type,req,len,rt,&reply,plen)==0);
    assert(reply.rpc.status==EFS_INODE_RPC_OK && reply.rpc.inode.ino==512);
    if (retained)assert(reply.known && reply.generation==9 && reply.shard==512);
}
int main(void) {
    struct rlimit limit={0,0};assert(!setrlimit(RLIMIT_CORE,&limit));
#ifdef __linux__
    /* RLIMIT_CORE does not suppress a piped core_pattern collector. These
     * intentional SIGABRT children need only their wait status, not apport
     * or systemd-coredump work outside the test's 30-second deadline. */
    assert(!prctl(PR_SET_DUMPABLE,0));
#endif
    for(unsigned retained=0;retained<2;retained++) {
        unsigned max=retained?7:5;
        for(unsigned mismatch=0;mismatch<=max;mismatch++) {
            pid_t pid=fork();assert(pid>=0);
            if(!pid){exercise(retained,mismatch);_exit(0);}
            int status;assert(waitpid(pid,&status,0)==pid);
            if(!mismatch)assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
            else assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGABRT);
        }
    }
    puts("CREATE fault hook: retained/legacy loss, BUSY, same durable identity and changed-identity refusal PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-create-fault-') as directory:
    path = Path(directory) / 'test.c'
    marker = Path(directory) / 'marker'
    marker.touch()
    path.write_text(code)
    extra = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('EFS_SANITIZE') == '1' else []
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'include'), *extra, str(path),
                    str(root/'libefs.a'), '-pthread', '-lm', '-ldl',
                    '-libverbs', '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True, timeout=30,
                   env=dict(os.environ, EFS_FAULT_CREATE_RETRY_FILE=str(marker)))
