#!/usr/bin/env python3
"""REPORT owner preparation: fresh authority, no redundant log entry, strict errors."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'src/server/raft_host.c').read_text()
a = s.index('static int host_namespace_session_prepare_owner(')
b = s.index('static int host_namespace_session_admit_covered', a)
pre = r'''
#include "efs/opid.h"
#include "efs/session.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define HOST_SESS_HDR_LEN 18
#define HOST_SESS_CREATE_LEN 22
#define EFS_MD_CMD_SESSION 1
#define EFS_MD_SESS_CREATE 2
#define HOST_SESS_GET 3
struct efs_raft_host {int local;};
struct efs_msg_raft_mkfs_reply {int leader_hint;uint64_t salt;};
int efs_opid_req_valid(const struct efs_opid_req*q){return q && q->id.seq && q->id.client_uuid[0];}
static int get_rc,create_rc,reads,creates;
static uint64_t answer;
static int host_hosts(struct efs_raft_host*h,uint8_t g){assert(g==2);return h->local;}
static uint32_t efs_kv_session_shard(const uint8_t*u){assert(u[0]==7);return 2;}
static uint8_t efs_raft_shard_group(uint32_t s){return s;}
static void wr32be(uint8_t*p,uint32_t n){for(int i=3;i>=0;i--){p[i]=(uint8_t)n;n>>=8;}}
static int host_session_get_covered(struct efs_raft_host*h,const uint8_t*c,uint32_t n,uint8_t g,int*hint,uint64_t*out,uint8_t covered){
 (void)h;assert(n==18 && g==2 && !covered && c[1]==HOST_SESS_GET && c[2]==7);
 reads++;*out=answer;*hint=3;return get_rc;
}
static int host_remote_cmd(struct efs_raft_host*h,uint8_t g,const uint8_t*c,uint32_t n,struct efs_msg_raft_mkfs_reply*r,int pref){
 assert(pref==-1);return host_session_get_covered(h,c,n,g,&r->leader_hint,&r->salt,0);
}
static int host_namespace_session_command(struct efs_raft_host*h,uint8_t g,const uint8_t*c,uint32_t n,int*hint){
 (void)h;(void)hint;assert(g==2 && n==22 && c[1]==EFS_MD_SESS_CREATE && c[21]==9);creates++;return create_rc;
}
'''
post = r'''
int main(void){
 struct efs_opid_req q={.id={.client_uuid={7},.session_epoch=9,.seq=1}};
 for(int local=0;local<=1;local++){
  struct efs_raft_host h={local};int hint=-1;
  reads=creates=0;get_rc=create_rc=0;answer=9|((uint64_t)EFS_SESSION_ACTIVE<<32);
  assert(!host_namespace_session_prepare_owner(&h,&q,&hint));assert(reads==1&&!creates&&hint==3);
  answer=8|((uint64_t)EFS_SESSION_ACTIVE<<32);
  assert(host_namespace_session_prepare_owner(&h,&q,&hint)==EFS_ERR_STALE && !creates);
  answer=9|((uint64_t)EFS_SESSION_FENCING<<32);
  assert(host_namespace_session_prepare_owner(&h,&q,&hint)==EFS_ERR_BUSY && !creates);
  answer=9|(255ull<<32);
  assert(host_namespace_session_prepare_owner(&h,&q,&hint)==EFS_ERR_PROTO && !creates);
  get_rc=EFS_ERR_IO;assert(host_namespace_session_prepare_owner(&h,&q,&hint)==EFS_ERR_IO && !creates);
  get_rc=EFS_ERR_NOT_FOUND;assert(!host_namespace_session_prepare_owner(&h,&q,&hint) && creates==1);
  create_rc=EFS_ERR_BUSY;assert(host_namespace_session_prepare_owner(&h,&q,&hint)==EFS_ERR_BUSY && creates==2);
  q.ack=1;int before=reads;assert(host_namespace_session_prepare_owner(&h,&q,&hint)==EFS_ERR_INVAL && reads==before);q.ack=0;
 }
 puts("REPORT owner: fresh local/remote reads, no existing-owner CREATE, absence-only creation, fencing and error propagation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-owner-') as d:
    c=Path(d)/'test.c'; c.write_text(pre+s[a:b]+post)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(c),'-o',str(c.with_suffix(''))],check=True)
    subprocess.run([str(c.with_suffix(''))],check=True)
