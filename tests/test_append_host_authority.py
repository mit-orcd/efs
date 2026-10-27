#!/usr/bin/env python3
"""Production append authority admission: leader-only allocation replies and
the session-owned reservation path (K5: no unidentified reservation and no
in-memory replay cache for one)."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
def fn(p):
 a=s.index(p);return s[a:s.index('\n}',a)+2]
code=r"""
#include "efs/protocol.h"
#include "efs/kv_key.h"
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define EFS_RAFT_LEADER 1
struct efs_raft{int role,leader;};
struct efs_raft_host{int raft_id;struct efs_kv *kv;pthread_mutex_t mu;struct efs_raft r;};
static struct efs_raft_host *g_host;
static struct efs_raft *group_raft(struct efs_raft_host*h,uint8_t g){return g==2?&h->r:NULL;}
static int efs_raft_role(struct efs_raft*r){return r->role;}
static int efs_raft_leader(struct efs_raft*r){return r->leader;}
"""+fn('static efs_node_id_t host_reply_hint(')+fn('static int host_append_primary(')+r"""
static struct efs_raft_host host={.mu=PTHREAD_MUTEX_INITIALIZER};
int main(void){g_host=&host;struct efs_msg_inode_reply r={0};host.r.role=0;host.r.leader=2;
 assert(!host_append_primary(&host,2,&r)&&r.status==EFS_INODE_RPC_NOT_PRIMARY&&r.primary_id==3);
 host.r.role=EFS_RAFT_LEADER;assert(host_append_primary(&host,2,&r));
 host.r.role=0;host.r.leader=-1;assert(!host_append_primary(&host,2,&r)&&!r.primary_id);
 assert(!host_append_primary(&host,0,&r));
 host.r.leader=0;assert(!host_append_primary(&host,2,&r)&&!r.primary_id);
 return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='efs-append-host-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=20)
# A term change cannot fall through to generic forwarding without the offset.
a=s.index('static int host_propose_impl(struct');b=s.index('rc = host_remote_cmd(h, group, cmd, clen, &rep, lid);',a)
assert 'cmd[0] == EFS_MD_CMD_APPEND_RSV' in s[a:b]
# K5: the owner refuses an append without uuid/epoch/seq/ACK and has no
# unidentified or cached-replay reservation path.
a=s.index('static void host_append_owner(');b=s.index('\n}',a);owner=s[a:b]
assert 'if (!ack || !sess_uuid)' in owner and 'EFS_ERR_UNREGISTERED' in owner
assert 'host_append_replay_get' not in s and 'ap_rep' not in s
print('append host: leader-only allocation replies, role-change forwarding guard and session-owned reservation PASS')

a=s.index('void server_raft_host_append(');b=s.index('\n}',a)
public=s[a:b]
assert public.index('host_fwd_append(')<public.index('pthread_mutex_trylock(lock)')
assert public.index('pthread_mutex_trylock(lock)')<public.index('host_append_owner(')<public.index('pthread_mutex_unlock(lock)')
