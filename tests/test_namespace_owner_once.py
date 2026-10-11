#!/usr/bin/env python3
"""Actual shared-owner admission preserves every participant authority check."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_NAMESPACE_OWNER_SOURCE',root/'src/server/raft_host.c')).read_text()
# Only the reviewed same-request namespace callers opt in. REPORT and LINK
# retain their independent admission read. The full live partition suites
# validate these callers against real stale authority.
assert s.count('host_namespace_session_admit_after_group(h,dsh,q,0,') == 5
a=s.index('static int host_namespace_session_prepare_owner(')
b=s.index('/* REPORT can first-touch',a)
code=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define EFS_OK 0
#define EFS_ERR_INVAL -1
#define EFS_ERR_NOT_FOUND -4
#define EFS_ERR_IO -6
#define EFS_ERR_BUSY -13
#define EFS_ERR_STALE -14
#define EFS_SESSION_BITS 64
#define EFS_KV_KEY_MAX 64
#define EFS_SESS_LOCAL_VAL 8
#define HOST_SESS_CREATE_LEN 22
#define HOST_SESS_REG_LEN 26
#define EFS_MD_CMD_SESSION 1
#define EFS_MD_SESS_CREATE 2
#define EFS_MD_SESS_REGISTER 3
#define EFS_MD_SESS_ESTABLISH 4
struct efs_raft_host {void *kv;};
struct efs_opid {uint8_t client_uuid[16];uint32_t session_epoch;uint64_t seq;};
struct efs_opid_req {struct efs_opid id;uint64_t ack;};
static unsigned creates,registers,establishes,reads,admit_checks,accepts;
static int read_error,owner_error,local_error,establish_error,fenced;
static uint8_t floors[64],registered[64];
static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void wr32be(uint8_t *p,uint32_t v) {p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static int efs_opid_req_valid(const struct efs_opid_req *q) {return q->id.seq && q->id.client_uuid[0];}
static uint8_t efs_raft_shard_group(uint32_t shard) {return shard%2?2:0;}
static uint32_t efs_kv_session_shard(const uint8_t *uuid) {assert(uuid[0]==19);return 8;}
static int host_read_index(struct efs_raft_host *h,uint8_t group,int *hint) {(void)h;assert(group==0||group==2);*hint=1;reads++;return read_error;}
static int efs_kv_key_sess_local(uint32_t shard,const uint8_t *uuid,uint8_t *key,uint32_t *length) {assert(uuid[0]==19);key[0]=shard;*length=1;return 0;}
static int efs_kv_get(void *kv,const uint8_t *key,uint32_t length,uint8_t *value,uint32_t *size) {(void)kv;(void)value;assert(length==1&&*size==8);return local_error?local_error:floors[key[0]]?0:EFS_ERR_NOT_FOUND;}
static int efs_session_accept(void *kv,uint32_t shard,const uint8_t *uuid,uint32_t epoch) {(void)kv;assert(uuid[0]==19&&epoch==3);accepts++;return fenced?EFS_ERR_STALE:floors[shard]?0:EFS_ERR_BUSY;}
static int host_namespace_session_command(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t length,int *hint) {
 (void)h;*hint=1;assert(cmd[0]==EFS_MD_CMD_SESSION&&cmd[2]==19&&rd32(cmd+18)==3);
 if(cmd[1]==EFS_MD_SESS_CREATE) {assert(group==0&&length==22);creates++;return owner_error;}
 assert(length==26);uint32_t shard=rd32(cmd+22);assert(shard<64);
 if(cmd[1]==EFS_MD_SESS_REGISTER) {assert(group==0);registers++;if(fenced)return EFS_ERR_STALE;registered[shard]=1;return 0;}
 assert(cmd[1]==EFS_MD_SESS_ESTABLISH&&group==efs_raft_shard_group(shard)&&registered[shard]);
 establishes++;floors[shard]=1;return 0;
}
static int host_session_establish_admit(struct efs_raft_host *h,const uint8_t *cmd,uint32_t length,uint8_t group,int *hint) {
 (void)h;*hint=1;assert(length==26&&cmd[1]==EFS_MD_SESS_ESTABLISH&&rd32(cmd+18)==3);
 uint32_t shard=rd32(cmd+22);assert(group==efs_raft_shard_group(shard)&&registered[shard]);admit_checks++;
 return establish_error?establish_error:fenced?EFS_ERR_STALE:0;
}
''' +s[a:b]+r'''
static void reset(void) {creates=registers=establishes=reads=admit_checks=accepts=0;read_error=owner_error=local_error=establish_error=fenced=0;memset(floors,0,sizeof(floors));memset(registered,0,sizeof(registered));}
int main(void) {
 struct efs_raft_host h={0};struct efs_opid_req q={.id={.client_uuid={19},.session_epoch=3,.seq=1}};int hint=-1;
 reset();assert(!host_namespace_session_prepare_owner(&h,&q,&hint));
 for(unsigned shard=1;shard<=4;shard++)assert(!host_namespace_session_admit_with_owner(&h,shard,&q,1,&hint));
 assert(creates==1&&registers==4&&establishes==4&&admit_checks==4&&reads==8&&accepts==4);
 reset();assert(!host_namespace_session_admit(&h,5,&q,&hint));
 assert(creates==1&&registers==1&&establishes==1&&admit_checks==1&&reads==2&&accepts==1);
 reset();floors[5]=1;assert(!host_namespace_session_admit(&h,5,&q,&hint));assert(!creates&&!registers&&!establishes&&reads==1&&accepts==1);
 reset();assert(!host_namespace_session_prepare_owner(&h,&q,&hint));fenced=1;
 assert(host_namespace_session_admit_with_owner(&h,5,&q,1,&hint)==EFS_ERR_STALE);assert(creates==1&&registers==1&&!establishes&&!accepts);
 for(int refusal=EFS_ERR_BUSY;refusal>=EFS_ERR_STALE;refusal--) {
  reset();assert(!host_namespace_session_prepare_owner(&h,&q,&hint));establish_error=refusal;
  assert(host_namespace_session_admit_with_owner(&h,5,&q,1,&hint)==refusal);assert(creates==1&&registers==1&&admit_checks==1&&!establishes&&!accepts);
 }
 reset();read_error=EFS_ERR_BUSY;assert(host_namespace_session_admit_with_owner(&h,5,&q,1,&hint)==EFS_ERR_BUSY);assert(!creates&&!registers&&!establishes&&!accepts);
 reset();local_error=EFS_ERR_IO;assert(host_namespace_session_admit_with_owner(&h,5,&q,1,&hint)==EFS_ERR_IO);assert(!creates&&!registers&&!establishes&&!accepts);
 reset();owner_error=EFS_ERR_IO;assert(host_namespace_session_admit(&h,5,&q,&hint)==EFS_ERR_IO);assert(creates==1&&!registers&&!establishes&&!accepts);
 /* Same-request CREATE proof skips only the matching initial group read. */
 reset();floors[5]=1;
 assert(!host_namespace_session_admit_after_group(&h,5,&q,0,2,&hint));
 assert(reads==0&&accepts==1&&!creates&&!registers&&!establishes);
 reset();floors[5]=1;fenced=1;
 assert(host_namespace_session_admit_after_group(&h,5,&q,0,2,&hint)==EFS_ERR_STALE);
 assert(reads==0&&accepts==1&&!creates&&!registers&&!establishes);
 reset();assert(!host_namespace_session_admit_after_group(&h,5,&q,0,2,&hint));
 assert(reads==1&&creates==1&&registers==1&&establishes==1&&admit_checks==1&&accepts==1);
 reset();read_error=EFS_ERR_BUSY;
 assert(host_namespace_session_admit_after_group(&h,5,&q,0,0,&hint)==EFS_ERR_BUSY);
 assert(reads==1&&!accepts&&!creates&&!registers&&!establishes);
 reset();floors[5]=1;local_error=EFS_ERR_IO;
 assert(host_namespace_session_admit_after_group(&h,5,&q,0,2,&hint)==EFS_ERR_IO);
 assert(!reads&&!accepts&&!creates&&!registers&&!establishes);
 reset();q.ack=1;assert(host_namespace_session_admit_with_owner(&h,5,&q,1,&hint)==EFS_ERR_INVAL);assert(!reads&&!creates&&!registers);
 puts("shared session owner: one CREATE, per-shard fresh reads/REGISTER/ESTABLISH/floor checks, fencing and hard-error refusal PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-owner-once-') as td:
 p=Path(td)/'check.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',str(p),'-pthread','-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=30)
