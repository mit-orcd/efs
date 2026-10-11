#!/usr/bin/env python3
"""Actual host append admission: global errors/fencing never become permission."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('static int host_append_session_admit_covered(');b=s.index('static int host_pack_session_cleanup(',a);f=s[a:b]
c=r'''
#include "efs/meta_cmd.h"
#include "efs/protocol.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define HOST_SESS_HDR_LEN 18
#define HOST_SESS_GET 0
struct efs_raft_host {struct efs_kv *kv;};
static int global_rc,local_rc,local_get,hosted=1,remote_calls,owner_reads;
static uint64_t global_salt;
static void wr32be(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=n>>(24-8*i);}
uint32_t efs_kv_session_shard(const uint8_t uuid[EFS_OPID_UUID_LEN]){assert(uuid[0]==42);return 2;}
static int host_hosts(struct efs_raft_host *h,uint8_t group){(void)h;assert(group==2);return hosted;}
static int host_session_get_covered(struct efs_raft_host *h,const uint8_t *p,uint32_t n,uint8_t g,int *hint,uint64_t *salt,uint8_t covered)
{if(!(covered&(1u<<g)))owner_reads++;(void)h;(void)hint;assert(n==22 && g==2 && p[1]==0 && p[2]==42 && p[21]==1);*salt=global_salt;return global_rc;}
static int host_remote_cmd(struct efs_raft_host *h,uint8_t g,const uint8_t *p,uint32_t n,struct efs_msg_raft_mkfs_reply *r,int target)
{(void)h;(void)g;(void)p;(void)n;assert(target==-1);remote_calls++;r->salt=global_salt;return global_rc;}
int efs_session_accept(struct efs_kv *kv,uint32_t shard,const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch)
{(void)kv;assert(shard==1 && uuid[0]==42 && epoch==1);return local_rc;}
int efs_kv_key_sess_local(uint32_t shard,const uint8_t uuid[EFS_OPID_UUID_LEN],uint8_t *key,uint32_t *len)
{assert(shard==1 && uuid[0]==42);memset(key,0,23);key[2]=EFS_KV_KIND_SESS_LOCAL;*len=23;return EFS_OK;}
static int get(void *ctx,const uint8_t *key,uint32_t kl,uint8_t *value,uint32_t *vl)
{(void)ctx;(void)value;(void)vl;assert(kl==23 && key[2]==EFS_KV_KIND_SESS_LOCAL);return local_get;}
'''+f+r'''
int main(void) {
 const struct efs_kv_ops ops={.get=get};struct efs_kv kv={.ops=&ops};struct efs_raft_host h={.kv=&kv};
 struct efs_opid_req q={.id={.client_uuid={42},.session_epoch=1,.seq=1}};int hint=-1,known=99;
 global_rc=EFS_ERR_IO;assert(host_append_session_admit(&h,4097,&q,&hint,&known)==EFS_ERR_IO && !known);
 global_rc=EFS_ERR_NOT_FOUND;local_get=EFS_ERR_NOT_FOUND;
 assert(host_append_session_admit(&h,4097,&q,&hint,&known)==EFS_OK && !known);
 local_get=EFS_OK;assert(host_append_session_admit(&h,4097,&q,&hint,&known)==EFS_ERR_NOT_FOUND && !known);
 global_rc=EFS_OK;global_salt=(uint64_t)EFS_SESSION_FENCING<<32|1;
 assert(host_append_session_admit(&h,4097,&q,&hint,&known)==EFS_ERR_BUSY && !known);
 global_salt=(uint64_t)EFS_SESSION_ACTIVE<<32|2;
 assert(host_append_session_admit(&h,4097,&q,&hint,&known)==EFS_ERR_STALE && !known);
 global_salt=(uint64_t)EFS_SESSION_ACTIVE<<32|1;local_rc=EFS_ERR_STALE;
 assert(host_append_session_admit(&h,4097,&q,&hint,&known)==EFS_ERR_STALE && !known);
 local_rc=EFS_ERR_BUSY;assert(host_append_session_admit(&h,4097,&q,&hint,&known)==EFS_ERR_BUSY && !known);
 local_rc=EFS_OK;assert(host_append_session_admit(&h,4097,&q,&hint,&known)==EFS_OK && known);
 int before=owner_reads;
 assert(host_append_session_admit_covered(&h,4097,&q,&hint,&known,1u<<2)==EFS_OK && known && owner_reads==before);
 global_salt=(uint64_t)EFS_SESSION_FENCING<<32|1;
 assert(host_append_session_admit_covered(&h,4097,&q,&hint,&known,1u<<2)==EFS_ERR_BUSY && !known && owner_reads==before);
 global_salt=(uint64_t)EFS_SESSION_ACTIVE<<32|1;local_rc=EFS_ERR_STALE;
 assert(host_append_session_admit_covered(&h,4097,&q,&hint,&known,1u<<2)==EFS_ERR_STALE && !known);
 local_rc=EFS_OK;
 assert(!host_append_session_admit_covered(&h,4097,&q,&hint,&known,1u<<0) && owner_reads==before+1);
 hosted=0;global_rc=EFS_ERR_NET;
 assert(host_append_session_admit(&h,4097,&q,&hint,&known)==EFS_ERR_NET && !known && remote_calls==1);
 puts("append host: authoritative global epoch, fencing/unknown/error refusal, local floor and legacy-only absence compatibility PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-host-') as t:
 p=Path(t);(p/'test.c').write_text(c)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
