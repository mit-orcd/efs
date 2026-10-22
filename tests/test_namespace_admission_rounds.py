#!/usr/bin/env python3
"""Fresh owner authority admits a namespace participant; hosts never create a
session for an unknown uuid (K5), so an absent owner is UNREGISTERED and a
different owner epoch is FENCED."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_namespace_session_admit_covered(');b=s.index('\n}',s.index('static int host_namespace_session_admit(',a))+2
code=r'''
#include "efs/session.h"
#include "efs/opid.h"
#include "efs/raft.h"
#include "efs/meta_cmd.h"
#include "efs/kv_key.h"
#define efs_kv_get test_kv_get
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define HOST_SESS_HDR_LEN 18
#define HOST_SESS_CREATE_LEN 22
#define HOST_SESS_REG_LEN 26
#define HOST_SESS_GET 0
struct efs_raft_host {struct efs_kv *kv;};
struct efs_msg_raft_mkfs_reply {int leader_hint;uint64_t salt;};
static int local_floor,accept_rc,local_error,owner_rc,owner_hosted=1,est_rc,command_rc;
static uint64_t owner_value;
static unsigned creates,registers,establishes,owner_reads,est_checks,accepts,fences,combined;static int read_error;
static void wr32be(uint8_t *p,uint32_t n){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static int host_read_index(struct efs_raft_host*h,uint8_t group,int *hint){(void)h;(void)group;(void)hint;fences++;return read_error?EFS_ERR_IO:0;}
int efs_kv_key_sess_local(uint32_t sh,const uint8_t id[EFS_OPID_UUID_LEN],uint8_t *key,uint32_t *kl){(void)sh;(void)id;key[0]=0;*kl=1;return 0;}
static int efs_kv_get(void *kv,const uint8_t *key,uint32_t kl,uint8_t *v,uint32_t *vl){(void)kv;(void)key;(void)kl;(void)v;(void)vl;return local_error?local_error:(local_floor?0:EFS_ERR_NOT_FOUND);}
int efs_session_accept(struct efs_kv *kv,uint32_t sh,const uint8_t id[EFS_OPID_UUID_LEN],uint32_t epoch){(void)kv;(void)sh;(void)id;(void)epoch;accepts++;return accept_rc;}
uint32_t efs_kv_session_shard(const uint8_t id[EFS_OPID_UUID_LEN]){(void)id;return 1;}
static int host_hosts(struct efs_raft_host *h,uint8_t group){(void)h;assert(group==0);return owner_hosted;}
static int host_session_get(struct efs_raft_host *h,const uint8_t *q,uint32_t n,uint8_t group,int *hint,uint64_t *out){(void)h;(void)group;(void)hint;assert(q[1]==0 && n==22);owner_reads++;*out=owner_value;return owner_rc;}
static int host_remote_cmd(struct efs_raft_host *h,uint8_t group,const uint8_t *q,uint32_t n,struct efs_msg_raft_mkfs_reply *r,int rid){(void)rid;r->leader_hint=1;return host_session_get(h,q,n,group,NULL,&r->salt);}
static int host_namespace_session_command(struct efs_raft_host *h,uint8_t group,const uint8_t *q,uint32_t n,int *hint){(void)h;(void)group;(void)hint;
 if(q[1]==EFS_MD_SESS_CREATE){assert(n==22);creates++;}
 else if(q[1]==EFS_MD_SESS_REGISTER_ESTABLISH){assert(n==26 && group==0);combined++;}
 else if(q[1]==EFS_MD_SESS_REGISTER){assert(n==26);registers++;}
 else {assert(q[1]==EFS_MD_SESS_ESTABLISH && n==26);establishes++;}return command_rc;}
static int host_session_establish_admit_covered(struct efs_raft_host *h,const uint8_t *q,uint32_t n,uint8_t group,int *hint,uint8_t covered){(void)h;(void)q;(void)n;(void)group;(void)hint;(void)covered;est_checks++;return est_rc;}
int efs_opid_req_valid(const struct efs_opid_req *q){return q!=NULL;}
static int host_session_get_covered(struct efs_raft_host *h,const uint8_t *q,uint32_t n,uint8_t group,int *hint,uint64_t *out,uint8_t covered){if(!(covered&(1u<<group))){int rc=host_read_index(h,group,hint);if(rc)return rc;}return host_session_get(h,q,n,group,hint,out);}
'''+s[a:b]+r'''
static void reset(void){local_floor=accept_rc=local_error=owner_rc=est_rc=command_rc=0;owner_value=7ull|((uint64_t)EFS_SESSION_ACTIVE<<32);creates=registers=establishes=owner_reads=est_checks=accepts=fences=combined=0;read_error=0;}
int main(void){struct efs_raft_host h={0};struct efs_opid_req q={0};int hint=-1;q.id.seq=1;q.id.session_epoch=7;
 for(unsigned remote=0;remote<2;remote++){owner_hosted=!remote;
 reset();assert(!host_namespace_session_admit(&h,2,&q,&hint));assert(!creates && registers==1 && establishes==1 && owner_reads==1 && est_checks==1 && accepts==1);
 reset();owner_value|=1ull<<40;assert(!host_namespace_session_admit(&h,2,&q,&hint));assert(!creates && !registers && establishes==1 && est_checks==1);
 reset();owner_rc=EFS_ERR_NOT_FOUND;assert(host_namespace_session_admit(&h,2,&q,&hint)==EFS_ERR_UNREGISTERED);assert(!creates && !registers && !establishes && !accepts);
 reset();owner_rc=EFS_ERR_IO;assert(host_namespace_session_admit(&h,2,&q,&hint)==EFS_ERR_IO && !creates && !registers && !establishes);
 reset();owner_value=8ull|((uint64_t)EFS_SESSION_ACTIVE<<32);assert(host_namespace_session_admit(&h,2,&q,&hint)==EFS_ERR_FENCED && !registers && !establishes);
 reset();owner_value=7ull|((uint64_t)EFS_SESSION_FENCING<<32);assert(host_namespace_session_admit(&h,2,&q,&hint)==EFS_ERR_BUSY && !registers && !establishes);
 reset();owner_value=7;assert(host_namespace_session_admit(&h,2,&q,&hint)==EFS_ERR_PROTO && !registers && !establishes);
 reset();owner_value|=1ull<<40;est_rc=EFS_ERR_BUSY;assert(host_namespace_session_admit(&h,2,&q,&hint)==EFS_ERR_BUSY && est_checks==1 && !establishes && !accepts);
 reset();local_floor=1;assert(!host_namespace_session_admit(&h,2,&q,&hint) && !owner_reads && accepts==1);
 }
 owner_hosted=1;reset();
 assert(!host_namespace_session_admit(&h,1,&q,&hint) && fences==1 && combined==1 && !registers && !establishes && !est_checks); /* one atomic local group */
 assert(!host_namespace_session_admit(&h,1,&q,&hint) && fences==2); /* independent RPC is fresh */
 reset();assert(!host_namespace_session_admit(&h,2,&q,&hint) && fences==2); /* new owner group */
 reset();assert(!host_namespace_session_admit_covered(&h,2,&q,&hint,5) && !fences); /* caller already fenced both */
 reset();command_rc=EFS_ERR_STALE;assert(host_namespace_session_admit(&h,1,&q,&hint)==EFS_ERR_STALE && combined==1 && !accepts && !registers && !establishes);
 reset();read_error=1;assert(host_namespace_session_admit(&h,2,&q,&hint)==EFS_ERR_IO && !creates && !registers && !establishes && !owner_reads);
 reset();read_error=1;assert(host_namespace_session_admit_covered(&h,2,&q,&hint,4)==EFS_ERR_IO && fences==1 && !registers && !establishes);
 reset();assert(!host_namespace_session_admit_with_owner(&h,2,&q,1,&hint) && owner_reads==1); /* prior CREATE is not cached ACTIVE authority */
 const int refusals[]={EFS_ERR_NOT_FOUND,EFS_ERR_INVAL,EFS_ERR_BUSY,EFS_ERR_STALE,EFS_ERR_IO,EFS_ERR_NOMEM};
 for(unsigned i=0;i<sizeof(refusals)/sizeof(refusals[0]);i++){
  reset();command_rc=refusals[i];assert(host_namespace_session_admit(&h,2,&q,&hint)==refusals[i]);assert(registers==1&&!establishes&&!accepts);
  reset();command_rc=refusals[i];assert(host_namespace_session_admit(&h,1,&q,&hint)==refusals[i]);assert(combined==1&&!accepts);
  reset();local_floor=1;accept_rc=refusals[i];assert(host_namespace_session_admit_covered(&h,2,&q,&hint,5)==refusals[i]);assert(!owner_reads&&!creates&&!registers&&!establishes);
 }
 reset();local_error=EFS_ERR_IO;assert(host_namespace_session_admit_with_owner(&h,2,&q,1,&hint)==EFS_ERR_IO);assert(!owner_reads&&!creates&&!registers&&!accepts);
 reset();est_rc=EFS_ERR_STALE;assert(host_namespace_session_admit(&h,2,&q,&hint)==EFS_ERR_STALE);assert(registers==1&&est_checks==1&&!establishes&&!accepts);
 assert(host_namespace_session_admit(&h,2,NULL,&hint)==EFS_ERR_UNREGISTERED); /* no identity */
 struct efs_opid_req none={0};assert(host_namespace_session_admit(&h,2,&none,&hint)==EFS_ERR_UNREGISTERED);
 puts("namespace admission: fresh local/remote authority, unknown owner and missing identity UNREGISTERED, ACTIVE/touched shortcuts, fenced/error rejection and final durable floor PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-namespace-admit-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
