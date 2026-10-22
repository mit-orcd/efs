#!/usr/bin/env python3
"""Actual atomic append adapter: exact authority, routing and error propagation."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
def fn(name):
 a=s.index('static int '+name+'(');return s[a:s.index('\n}',a)+2]
c=r'''
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
struct efs_raft_host{struct efs_kv *kv;uint64_t apply_extra;pthread_mutex_t mu;};
static int role=EFS_RAFT_LEADER;static uint32_t voters=7;
uint32_t efs_raft_voters(const struct efs_raft*r){assert(r);return voters;}
static struct efs_raft *group_raft(struct efs_raft_host*h,uint8_t group){(void)h;assert(group==efs_raft_shard_group(1));return (struct efs_raft*)1;}
int efs_raft_role(const struct efs_raft*r){assert(r);return role;}
static int decode_rc,apply_rc,encode_rc,propose_rc,wait_rc,applies,proposals,waits;
static unsigned decoded_shard=1;
int efs_meta_append_atomic_decode(const uint8_t*in,uint32_t n,struct efs_meta_append_plan*p,struct efs_opid_req*q){assert(in && n==8);memset(p,0,sizeof(*p));memset(q,0,sizeof(*q));p->parts.n=1;p->parts.shard[0]=decoded_shard;p->offset=123;return decode_rc;}
int efs_meta_apply_append_atomic(struct efs_kv*kv,const struct efs_meta_append_plan*p,const struct efs_opid_req*q){(void)kv;(void)q;assert(p->offset==123);applies++;return apply_rc;}
int efs_meta_append_atomic_encode(const struct efs_meta_append_plan*p,const struct efs_opid_req*q,uint8_t*out,uint32_t cap,uint32_t*n){(void)p;(void)q;assert(cap==EFS_META_APPEND_ATOMIC_MAX);memset(out,0,8);*n=8;return encode_rc;}
static int host_propose_mode(struct efs_raft_host*h,uint8_t group,const uint8_t*cmd,uint32_t n,uint64_t*index,uint64_t*term,int*hint,int local_only){(void)h;(void)hint;assert(group==efs_raft_shard_group(1) && cmd[0]==EFS_MD_CMD_APPEND_ATOMIC && n==9 && local_only==1);proposals++;*index=7;*term=3;return propose_rc;}
static int host_wait_verdict(struct efs_raft_host*h,uint8_t group,uint64_t index,uint64_t term,int*hint){(void)h;(void)hint;assert(group==efs_raft_shard_group(1) && index==7 && term==3);waits++;return wait_rc;}
''' + fn('apply_append_atomic_cmd')+fn('host_append_execute_atomic')+fn('host_append_atomic_eligible')+r'''
int main(void){struct efs_raft_host h={0};uint8_t cmd[9]={EFS_MD_CMD_APPEND_ATOMIC};int hint=-1;
 uint8_t group=efs_raft_shard_group(1);
 assert(apply_append_atomic_cmd(&h,cmd,1,group)==EFS_ERR_INVAL && !applies);
 decode_rc=EFS_ERR_PROTO;assert(apply_append_atomic_cmd(&h,cmd,9,group)==EFS_ERR_PROTO && !applies);
 decode_rc=0;assert(apply_append_atomic_cmd(&h,cmd,9,group==0?2:0)==EFS_ERR_INVAL && !applies);
 apply_rc=EFS_ERR_IO;assert(apply_append_atomic_cmd(&h,cmd,9,group)==EFS_ERR_IO && !h.apply_extra);
 apply_rc=EFS_ERR_STALE;assert(apply_append_atomic_cmd(&h,cmd,9,group)==EFS_ERR_STALE && !h.apply_extra);
 apply_rc=0;assert(!apply_append_atomic_cmd(&h,cmd,9,group) && h.apply_extra==123);
 struct efs_meta_append_plan *p=calloc(1,sizeof(*p));assert(p);p->parts.n=1;p->parts.shard[0]=1;struct efs_opid_req q={0};
 pthread_mutex_init(&h.mu,NULL);p->count=4;
 assert(host_append_atomic_eligible(&h,p));role=EFS_RAFT_FOLLOWER;
 assert(!host_append_atomic_eligible(&h,p));role=EFS_RAFT_LEADER;voters=1;
 assert(!host_append_atomic_eligible(&h,p));voters=7;p->count=0;
 assert(!host_append_atomic_eligible(&h,p));p->count=4;p->parts.n=2;
 assert(!host_append_atomic_eligible(&h,p));p->parts.n=1;
 encode_rc=EFS_ERR_INVAL;assert(host_append_execute_atomic(&h,p,&q,&hint)==EFS_ERR_INVAL && !proposals && !waits);
 encode_rc=0;propose_rc=EFS_ERR_NOT_PRIMARY;
 assert(host_append_execute_atomic(&h,p,&q,&hint)==EFS_ERR_NOT_PRIMARY && proposals==1 && !waits);
 propose_rc=0;wait_rc=EFS_ERR_IO;assert(host_append_execute_atomic(&h,p,&q,&hint)==EFS_ERR_IO && waits==1);
 wait_rc=0;assert(!host_append_execute_atomic(&h,p,&q,&hint) && waits==2);free(p);
 puts("atomic append host: same-group apply, exact offset, malformed/refused/error propagation and role-change local-only submission PASS");
}
'''
a=s.index('static int host_append_transaction(');b=s.index('\n}',a);body=s[a:b]
# K5: every append is session-owned; there is no unregistered branch.
assert 'host_append_atomic_eligible(h,plan)' in body
assert 'host_append_execute_batched(h,plan,q,&t,hint)' in body
assert 'registered' not in body
assert 'cmd[0] == EFS_MD_CMD_APPEND_ATOMIC' in s[s.index('ret = (cmd[0]'):s.index('/* Finish the atomic apply scope')]
with tempfile.TemporaryDirectory(prefix='efs-atomic-host-') as d:
 p=Path(d)/'t.c';p.write_text(c)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
