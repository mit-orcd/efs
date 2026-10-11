#!/usr/bin/env python3
"""Actual shared-owner admission preserves every participant authority check."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_NAMESPACE_OWNER_SOURCE',root/'src/server/raft_host.c')).read_text()
# Only the reviewed same-request namespace callers opt in. REPORT and LINK
# retain their independent admission read. The full live partition suites
# validate these callers against real stale authority.
assert s.count('host_namespace_session_admit_after_group(h,dsh,q,0,') == 5
a=s.index('static int host_namespace_session_register(') if 'static int host_namespace_session_register(' in s else s.index('static int host_namespace_session_prepare_owner(')
actual=(root/'src/server/raft_host.c').read_text()
producer=actual[actual.index('static uint64_t host_session_register_active_proof('):actual.index('static int apply_session_cmd(')]
b=s.index('/* REPORT can first-touch',a)
session=s[s.index('static int apply_session_cmd('):]
session=session[session.index('    if (required_failure)'):session.index('/* Same encoding as sim_lock_apply.')].rstrip()
# Include both production return policies: metadata business verdicts must
# reach admission, while Raft advances rejected commands without hiding rc.
outer=s[s.index('static int host_apply(void *app'):]
outer=outer[outer.index('    ret = (cmd[0]'):outer.index('    /* Finish the atomic apply scope')]
policies=session+outer
constants=sorted(set(re.findall(r'\b(?:EFS_MD_\w+|HOST_\w+)\b',policies))-
                 {'EFS_MD_CMD_SESSION','EFS_MD_SESS_CREATE','EFS_MD_SESS_REGISTER','EFS_MD_SESS_ESTABLISH','HOST_SESS_CREATE_LEN','HOST_SESS_REG_LEN'})
macros='\n'.join('#define '+name+' '+str(100+i) for i,name in enumerate(constants))+'\n'
code=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define EFS_OK 0
#define EFS_ERR_INVAL -1
#define EFS_ERR_NOT_FOUND -4
#define EFS_ERR_IO -6
#define EFS_ERR_NOMEM -7
#define EFS_ERR_PROTO -8
#define EFS_ERR_BUSY -13
#define EFS_ERR_STALE -14
#define EFS_SESSION_BITS 64
#define EFS_KV_KEY_MAX 64
#define EFS_SESS_LOCAL_VAL 8
#define HOST_SESS_HDR_LEN 18
#define HOST_SESS_GET 0
#define HOST_SESS_CREATE_LEN 22
#define HOST_SESS_REG_LEN 26
#define EFS_MD_CMD_SESSION 1
#define EFS_MD_SESS_CREATE 2
#define EFS_MD_SESS_REGISTER 3
#define EFS_MD_SESS_ESTABLISH 4
#define EFS_SESSION_ACTIVE 1
#define EFS_SESSION_FENCING 2
struct efs_session_rec {uint32_t epoch;uint8_t state,touched[8];};
struct efs_raft_host {void *kv;uint64_t apply_extra;};
struct efs_opid {uint8_t client_uuid[16];uint32_t session_epoch;uint64_t seq;};
struct efs_opid_req {struct efs_opid id;uint64_t ack;};
static unsigned creates,registers,establishes,reads,admit_checks,accepts;
static int read_error,owner_error,local_error,establish_error,fenced,owner_exists,register_error;
static uint8_t floors[64],registered[64];
static int local_owner=1,proof_read_error,corrupt_proof,fencing_idempotent,fence_after_register,fence_after_view;
static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
#define rd32be rd32
static void wr32be(uint8_t *p,uint32_t v) {p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static int efs_opid_req_valid(const struct efs_opid_req *q) {return q->id.seq && q->id.client_uuid[0];}
static uint8_t efs_raft_shard_group(uint32_t shard) {return shard%2?2:0;}
static uint32_t efs_kv_session_shard(const uint8_t *uuid) {assert(uuid[0]==19);return 8;}
static int host_read_index(struct efs_raft_host *h,uint8_t group,int *hint) {(void)h;assert(group==0||group==2);*hint=1;reads++;return read_error;}
static int efs_kv_key_sess_local(uint32_t shard,const uint8_t *uuid,uint8_t *key,uint32_t *length) {assert(uuid[0]==19);key[0]=shard;*length=1;return 0;}
static int efs_kv_get(void *kv,const uint8_t *key,uint32_t length,uint8_t *value,uint32_t *size) {(void)kv;(void)value;assert(length==1&&*size==8);return local_error?local_error:floors[key[0]]?0:EFS_ERR_NOT_FOUND;}
static int efs_session_accept(void *kv,uint32_t shard,const uint8_t *uuid,uint32_t epoch) {(void)kv;assert(uuid[0]==19&&epoch==3);accepts++;return fenced?EFS_ERR_STALE:floors[shard]?0:EFS_ERR_BUSY;}

static int efs_session_register(void *kv,const uint8_t *uuid,uint32_t epoch,uint32_t shard) {
 (void)kv;assert(uuid[0]==19&&epoch==3&&shard<64);
 if(register_error)return register_error;
 if(!owner_exists)return EFS_ERR_NOT_FOUND;
 if(fenced&&!fencing_idempotent)return EFS_ERR_STALE;
 registered[shard]=1;return EFS_OK;
}
static int efs_session_get(void *kv,const uint8_t *uuid,struct efs_session_rec *owner) {
 (void)kv;assert(uuid[0]==19);if(proof_read_error)return proof_read_error;if(!owner_exists)return EFS_ERR_NOT_FOUND;
 memset(owner,0,sizeof(*owner));owner->epoch=3;owner->state=fenced?EFS_SESSION_FENCING:EFS_SESSION_ACTIVE;
 for(unsigned n=0;n<64;n++)if(registered[n])owner->touched[n/8]|=1u<<(n%8);
 return EFS_OK;
}
static int efs_session_bit_get(const uint8_t *bits,uint32_t shard) {return !!(bits[shard/8]&(1u<<(shard%8)));}
''' +producer+macros+r'''
static int session_verdict(const uint8_t *cmd,int rc) {
 int required_failure=0;uint64_t index=41,ino=0;uint32_t shard=0;
''' +session+r'''
static int apply_verdict(const uint8_t *cmd,int rc,uint32_t clen) {int ret;
''' +outer+r'''
 return ret;
}
static int command_verdict(const uint8_t *cmd,int rc,uint32_t clen) {
 int verdict=session_verdict(cmd,rc);
 assert(apply_verdict(cmd,verdict,clen)==((verdict==EFS_ERR_IO||verdict==EFS_ERR_NOMEM)?verdict:EFS_OK));
 return verdict;
}
static int host_namespace_session_command(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t length,int *hint) {
 (void)h;*hint=1;assert(cmd[0]==EFS_MD_CMD_SESSION&&cmd[2]==19&&rd32(cmd+18)==3);
 if(cmd[1]==EFS_MD_SESS_CREATE) {assert(group==0&&length==22);creates++;if(owner_error)return owner_error;owner_exists=1;return 0;}
 assert(length==26);uint32_t shard=rd32(cmd+22);assert(shard<64);
 if(cmd[1]==EFS_MD_SESS_REGISTER) {assert(group==0);registers++;int rc=host_session_register_apply(h,cmd+2,rd32(cmd+18),shard);return command_verdict(cmd,rc,length);}
 assert(cmd[1]==EFS_MD_SESS_ESTABLISH&&group==efs_raft_shard_group(shard)&&registered[shard]);
 establishes++;floors[shard]=1;return 0;
}

static int __attribute__((unused)) host_hosts(struct efs_raft_host *h,uint8_t group) {(void)h;assert(group==0);return local_owner;}
static int __attribute__((unused)) host_propose_wait_ex(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t length,int *hint,uint64_t *extra) {
 int rc=host_namespace_session_command(h,group,cmd,length,hint);*extra=h->apply_extra;
 if(corrupt_proof)*extra^=1ull<<32;
 if(fence_after_register&&!rc)fenced=1;
 return rc;
}
static int host_session_establish_admit(struct efs_raft_host *h,const uint8_t *cmd,uint32_t length,uint8_t group,int *hint) {
 (void)h;*hint=1;assert(length==26&&cmd[1]==EFS_MD_SESS_ESTABLISH&&rd32(cmd+18)==3);
 uint32_t shard=rd32(cmd+22);assert(group==efs_raft_shard_group(shard)&&registered[shard]);admit_checks++;
 return establish_error?establish_error:fenced?EFS_ERR_STALE:0;
}

struct efs_msg_raft_mkfs_reply {int leader_hint;uint64_t salt;};
static int host_session_get(struct efs_raft_host *h,const uint8_t *q,uint32_t n,uint8_t group,int *hint,uint64_t *out) {
 (void)h;(void)hint;assert(group==0&&q[1]==HOST_SESS_GET&&n==22);
 if(!owner_exists)return EFS_ERR_NOT_FOUND;
 uint32_t shard=rd32(q+18);*out=3ull|((uint64_t)(fenced?EFS_SESSION_FENCING:EFS_SESSION_ACTIVE)<<32)|((uint64_t)registered[shard]<<40);if(fence_after_view)fenced=1;return EFS_OK;
}
static int host_remote_cmd(struct efs_raft_host *h,uint8_t group,const uint8_t *q,uint32_t n,struct efs_msg_raft_mkfs_reply *reply,int skip) {
 (void)skip;reply->leader_hint=1;return host_session_get(h,q,n,group,NULL,&reply->salt);
}
''' +s[a:b]+r'''
static void reset(void) {creates=registers=establishes=reads=admit_checks=accepts=0;read_error=owner_error=local_error=establish_error=fenced=owner_exists=register_error=0;local_owner=1;proof_read_error=corrupt_proof=fencing_idempotent=fence_after_register=fence_after_view=0;memset(floors,0,sizeof(floors));memset(registered,0,sizeof(registered));}
int main(void) {
 struct efs_raft_host h={0};struct efs_opid_req q={.id={.client_uuid={19},.session_epoch=3,.seq=1}};int hint=-1;
 reset();owner_exists=1;assert(!host_namespace_session_admit(&h,5,&q,&hint));
 assert(!creates&&registers==1&&establishes==1&&admit_checks==0&&reads==2&&accepts==1);
 for(int i=0;i<5;i++) {
  const int refusals[]={EFS_ERR_BUSY,EFS_ERR_STALE,EFS_ERR_IO,EFS_ERR_NOMEM,EFS_ERR_INVAL};
  reset();owner_exists=1;register_error=refusals[i];
  assert(host_namespace_session_admit(&h,5,&q,&hint)==refusals[i]);
  assert(!creates&&registers==1&&!establishes&&!admit_checks&&!accepts);
 }
 reset();register_error=EFS_ERR_IO;
 assert(host_namespace_session_admit(&h,5,&q,&hint)==EFS_ERR_IO);
 assert(creates==1&&registers==1&&!establishes&&!accepts);
 reset();assert(host_namespace_session_admit_with_owner(&h,5,&q,1,&hint)==EFS_ERR_NOT_FOUND);
 assert(!creates&&!registers&&!establishes&&!accepts);
 reset();register_error=EFS_ERR_NOT_FOUND;
 assert(host_namespace_session_admit(&h,5,&q,&hint)==EFS_ERR_NOT_FOUND);
 assert(creates==1&&registers==1&&!establishes&&!accepts);
 reset();assert(!host_namespace_session_prepare_owner(&h,&q,&hint));
 for(unsigned shard=1;shard<=4;shard++)assert(!host_namespace_session_admit_with_owner(&h,shard,&q,1,&hint));
 assert(creates==1&&registers==4&&establishes==4&&admit_checks==0&&reads==8&&accepts==4);
 reset();assert(!host_namespace_session_admit(&h,5,&q,&hint));
 assert(creates==1&&registers==1&&establishes==1&&admit_checks==0&&reads==2&&accepts==1);
 reset();floors[5]=1;assert(!host_namespace_session_admit(&h,5,&q,&hint));assert(!creates&&!registers&&!establishes&&reads==1&&accepts==1);
 reset();assert(!host_namespace_session_prepare_owner(&h,&q,&hint));fenced=1;
 assert(host_namespace_session_admit_with_owner(&h,5,&q,1,&hint)==EFS_ERR_BUSY);assert(creates==1&&!registers&&!establishes&&!accepts);
 for(int refusal=EFS_ERR_BUSY;refusal>=EFS_ERR_STALE;refusal--) {
  reset();assert(!host_namespace_session_prepare_owner(&h,&q,&hint));establish_error=refusal;local_owner=0;
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
 assert(reads==1&&creates==1&&registers==1&&establishes==1&&admit_checks==0&&accepts==1);
 reset();read_error=EFS_ERR_BUSY;
 assert(host_namespace_session_admit_after_group(&h,5,&q,0,0,&hint)==EFS_ERR_BUSY);
 assert(reads==1&&!accepts&&!creates&&!registers&&!establishes);
 reset();floors[5]=1;local_error=EFS_ERR_IO;
 assert(host_namespace_session_admit_after_group(&h,5,&q,0,2,&hint)==EFS_ERR_IO);
 assert(!reads&&!accepts&&!creates&&!registers&&!establishes);

 reset();owner_exists=1;fenced=fencing_idempotent=1;
 assert(host_namespace_session_admit(&h,5,&q,&hint)==EFS_ERR_BUSY);
 assert(!registers&&!admit_checks&&!establishes&&!accepts);
 reset();owner_exists=1;proof_read_error=EFS_ERR_IO;
 assert(!host_namespace_session_admit(&h,5,&q,&hint));assert(admit_checks==1&&!h.apply_extra);
 reset();owner_exists=1;corrupt_proof=1;
 assert(!host_namespace_session_admit(&h,5,&q,&hint));assert(admit_checks==1);
 reset();owner_exists=1;local_owner=0;
 assert(!host_namespace_session_admit(&h,5,&q,&hint));assert(admit_checks==1);
 reset();owner_exists=1;fence_after_register=1;
 assert(host_namespace_session_admit(&h,5,&q,&hint)==EFS_ERR_STALE);
 assert(registers==1&&admit_checks==0&&establishes==1&&reads==2&&accepts==1);
 reset();owner_exists=1;registered[5]=1;fence_after_view=1;
 assert(host_namespace_session_admit(&h,5,&q,&hint)==EFS_ERR_STALE);
 assert(!creates&&!registers&&establishes==1&&reads==2&&!admit_checks&&accepts==1);
 reset();q.ack=1;assert(host_namespace_session_admit_with_owner(&h,5,&q,1,&hint)==EFS_ERR_INVAL);assert(!reads&&!creates&&!registers);
 puts("shared session owner: actual ACTIVE apply proof, FENCING/unhosted/uncertain/mismatched fallback and post-apply fence refusal; existing-owner REGISTER without CREATE, authoritative missing-only bootstrap, per-shard fresh reads/ESTABLISH/floors, fencing and hard-error refusal PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-owner-once-') as td:
 p=Path(td)/'check.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',str(p),'-pthread','-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=30)
