#!/usr/bin/env python3
"""Production append authority admission and synchronized replay ownership."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
def fn(p):
 a=s.index(p);return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/protocol.h"
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define EFS_RAFT_LEADER 1
struct efs_raft{int role,leader;};
struct efs_raft_host{pthread_mutex_t mu;struct efs_raft r;struct {uint64_t token,seq;efs_ino_t ino;uint64_t off;}ap_rep[512];uint32_t ap_rep_i;};
static struct efs_raft *group_raft(struct efs_raft_host*h,uint8_t g){return g==2?&h->r:NULL;}
static int efs_raft_role(struct efs_raft*r){return r->role;}
static int efs_raft_leader(struct efs_raft*r){return r->leader;}
'''+fn('static int host_append_primary(')+fn('static int host_append_replay_get(')+fn('static void host_append_replay_put(')+r'''
static struct efs_raft_host host={.mu=PTHREAD_MUTEX_INITIALIZER};
static void *worker(void*p){uint64_t id=(uintptr_t)p;uint8_t uuid[16]={0};memcpy(uuid,&id,8);
 for(unsigned i=0;i<50000;i++){uint64_t seq=id*50000+i+1,out;
 host_append_replay_put(&host,id,uuid,seq,seq*8);
 if(!host_append_replay_get(&host,id,uuid,seq,&out))assert(out==seq*8);
 }return NULL;}
int main(void){struct efs_msg_inode_reply r={0};host.r.role=0;host.r.leader=2;
 assert(!host_append_primary(&host,2,&r)&&r.status==EFS_INODE_RPC_NOT_PRIMARY&&r.primary_id==3);
 host.r.role=EFS_RAFT_LEADER;assert(host_append_primary(&host,2,&r));
 host.r.role=0;host.r.leader=-1;assert(!host_append_primary(&host,2,&r)&&!r.primary_id);
 assert(!host_append_primary(&host,0,&r));
 pthread_t t[8];for(uintptr_t i=0;i<8;i++)assert(!pthread_create(&t[i],NULL,worker,(void*)(i+1)));
 for(int i=0;i<8;i++){assert(!pthread_join(t[i],NULL));}
 assert(host.ap_rep_i==400000);return 0;
}
'''
if os.environ.get('APPEND_HOST_TEST_OLD'):
 code=code.replace('pthread_mutex_lock(&h->mu);','').replace('pthread_mutex_unlock(&h->mu);','')
with tempfile.TemporaryDirectory(prefix='efs-append-host-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=20)
# A term change cannot fall through to generic forwarding without the offset.
a=s.index('static int host_propose(struct');b=s.index('rc = host_remote_cmd(h, group, cmd, clen, &rep, lid);',a)
assert 'cmd[0] == EFS_MD_CMD_APPEND_RSV' in s[a:b]
a=s.index('void server_raft_host_append(');b=s.index('host_append_replay_get(',a)
assert 'if (!host_append_primary(h, ig, out))' in s[a:b]
print('append host: leader-only allocation replies, role-change forwarding guard and concurrent replay ownership PASS')
