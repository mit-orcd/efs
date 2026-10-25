#!/usr/bin/env python3
"""Actual APPEND cleanup step certifies only definite no-op BUSY refusals:
read-only selection, and (K42) a step whose meta call names a wait reason."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=Path(os.environ.get('EFS_CLEANUP_SERVER_SOURCE',root/'src/server/raft_host.c')).read_text()
start=s.index('static int host_session_append_step(struct efs_raft_host *h,')
function=s[start:s.index('\n}',start)+2]
code=r'''
#include "efs/meta_cmd.h"
#include "efs/meta_apply.h"
#include "efs/session.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define HOST_SESS_REG_LEN 26
struct efs_raft_host {struct efs_kv *kv;};
struct host_resize_ctx {struct efs_raft_host *host;int *hint;};
static const struct efs_meta_append_ops host_append_ops={0};
static int admission,authority,selection,transaction;
static unsigned admit_calls,authority_calls,select_calls,txids,finishes,retires;
static unsigned present=1,progress,reason;
static uint32_t rd32be(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int host_session_cleanup_admit(struct efs_raft_host *h,const uint8_t *p,
 uint32_t n,uint8_t group,int *hint)
{assert(h && !h->kv && n==26 && group==2 && hint && p[0]==EFS_MD_CMD_SESSION && p[1]==EFS_MD_SESS_LEASE_LOCK_NEXT && p[2]==42 && rd32be(p+18)==1 && rd32be(p+22)==7);admit_calls++;return admission;}
static int host_append_authority(struct efs_raft_host *h,int *hint)
{assert(h && hint);authority_calls++;return authority;}
int efs_meta_append_session_next(struct efs_kv *kv,uint32_t shard,
 const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch,efs_ino_t *ino,
 uint64_t *generation,uint64_t *offset,unsigned *found)
{assert(!kv && shard==7 && uuid[0]==42 && epoch==1);select_calls++;*ino=4097;*generation=9;*offset=123;*found=present;return selection;}
static void fill_txid(struct efs_raft_host *h,struct efs_txid *tx)
{assert(h && tx);memset(tx,0,sizeof(*tx));txids++;}
int efs_meta_finish_append_fenced(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,
 uint64_t off,const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch,
 const struct efs_txid *tx,const struct efs_meta_append_ops *ops,void *user,unsigned *wait)
{struct host_resize_ctx *ctx=user;assert(!kv && ino==4097 && gen==9 && off==123 && uuid[0]==42 && epoch==1 && tx && ops==&host_append_ops && ctx->host && ctx->hint);finishes++;*wait=reason;return transaction;}
int efs_meta_retire_append_session(struct efs_kv *kv,uint32_t shard,
 const uint8_t uuid[EFS_OPID_UUID_LEN],uint32_t epoch,const struct efs_txid *tx,
 const struct efs_meta_append_ops *ops,void *user,unsigned *done,unsigned *wait)
{assert(!kv && shard==7 && uuid[0]==42 && epoch==1 && tx && ops==&host_append_ops && user);retires++;*done=progress;*wait=reason;return transaction;}
'''+function+r'''
static void reset(void) {
 admission=authority=selection=transaction=EFS_OK;
 admit_calls=authority_calls=select_calls=txids=finishes=retires=progress=reason=0;
 present=1;
}
int main(void) {
 struct efs_raft_host h={0};int hint=-1;uint64_t salt;
 uint8_t cmd[25]={EFS_MD_REQ_SESSION_APPEND_NEXT};cmd[1]=42;cmd[20]=1;cmd[24]=7;
 int errors[]={EFS_ERR_BUSY,EFS_ERR_NOT_PRIMARY,EFS_ERR_IO,EFS_ERR_NET,EFS_ERR_NOMEM,EFS_ERR_STALE,EFS_ERR_INVAL,EFS_ERR_PROTO};
 for(unsigned stage=0;stage<3;stage++)for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
  reset();salt=99;
  if(stage==0)admission=errors[i];else if(stage==1)authority=errors[i];else selection=errors[i];
  assert(host_session_append_step(&h,cmd,sizeof(cmd),2,&hint,&salt)==errors[i]);
  assert(salt==(errors[i]==EFS_ERR_BUSY?EFS_MD_SESSION_APPEND_RETRY_SALT:0));
  assert(admit_calls==1 && authority_calls==(stage>=1) && select_calls==(stage==2) && !txids && !finishes && !retires);
 }
 reset();salt=99;
 assert(host_session_append_step(&h,cmd,24,2,&hint,&salt)==EFS_ERR_INVAL && !salt && !admit_calls);
 for(present=0;present<2;present++) {
  unsigned have=present;
  for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
   reset();present=have;transaction=errors[i];salt=99;
   assert(host_session_append_step(&h,cmd,sizeof(cmd),2,&hint,&salt)==errors[i] && !salt);
   assert(txids==1 && finishes==have && retires==!have);
  }
  reset();present=have;transaction=EFS_ERR_NOT_FOUND;salt=99;
  assert(host_session_append_step(&h,cmd,sizeof(cmd),2,&hint,&salt)==EFS_ERR_BUSY && !salt);
  /* K42: a named wait certifies a BUSY (or a REAP-owed NOT_FOUND) only. */
  for(unsigned w=EFS_META_APPEND_WAIT_OPEN;w<=EFS_META_APPEND_WAIT_GONE;w++) {
   reset();present=have;reason=w;transaction=EFS_ERR_BUSY;salt=99;
   assert(host_session_append_step(&h,cmd,sizeof(cmd),2,&hint,&salt)==EFS_ERR_BUSY);
   assert(salt==EFS_MD_SESSION_APPEND_RETRY_SALT+w && salt<=EFS_MD_SESSION_APPEND_WAIT_GONE);
   reset();present=have;reason=w;transaction=EFS_ERR_NOT_FOUND;salt=99;
   assert(host_session_append_step(&h,cmd,sizeof(cmd),2,&hint,&salt)==EFS_ERR_BUSY && salt==EFS_MD_SESSION_APPEND_RETRY_SALT+w);
   for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
    if(errors[i]==EFS_ERR_BUSY)continue;
    reset();present=have;reason=w;transaction=errors[i];salt=99;
    assert(host_session_append_step(&h,cmd,sizeof(cmd),2,&hint,&salt)==errors[i] && !salt);
   }
  }
  reset();present=have;salt=99;
  assert(host_session_append_step(&h,cmd,sizeof(cmd),2,&hint,&salt)==EFS_OK && salt==!have);
  present=have;
 }
 reset();present=0;progress=1;salt=99;
 assert(host_session_append_step(&h,cmd,sizeof(cmd),2,&hint,&salt)==EFS_OK && !salt && retires==1);
 puts("APPEND cleanup: readonly and named-wait certificates, immutable identity, transaction/transport ambiguity refused, exact empty/progress verdict PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-cleanup-') as directory:
    path=Path(directory)/'test.c';path.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
