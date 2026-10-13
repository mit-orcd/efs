#!/usr/bin/env python3
"""Production single-reservation selector: coverage, identity and exact verdict."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('static int host_complete_single_append(');b=s.index('\n}',a)+2
code=r'''
#include "efs/meta_cmd.h"
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define EFS_KV_KEY_MAX 64
#define EFS_RAFT_LEADER 2
struct efs_raft {int role;uint32_t voters;};
struct efs_raft_host {void *kv;pthread_mutex_t mu;};
struct efs_meta_row {uint64_t generation,content_epoch;};
struct efs_opid {uint8_t client_uuid[16];uint32_t session_epoch;};
static struct efs_raft raft={2,7};static int eligible=1,lookup_rc,submit_rc,verdict_rc;
static unsigned submissions,verdicts;static uint32_t open_count=1,stored_length=48;
static uint64_t offset=100,length=8;static uint8_t reservation[48];
static struct efs_raft *group_raft(struct efs_raft_host*h,uint8_t g){(void)h;assert(g==0);return &raft;}
static uint32_t efs_raft_voters(struct efs_raft*r){return r->voters;}
static int efs_raft_role(struct efs_raft*r){return r->role;}
static uint32_t efs_kv_inode_shard(uint64_t ino){assert(ino==11);return 1;}
static uint8_t efs_raft_shard_group(uint32_t shard){assert(shard==1);return 0;}
static uint64_t rd64be(const uint8_t*p){uint64_t n=0;for(int i=0;i<8;i++)n=n<<8|p[i];return n;}
static uint32_t rd32be(const uint8_t*p){uint32_t n=0;for(int i=0;i<4;i++)n=n<<8|p[i];return n;}
static void wr64be(uint8_t*p,uint64_t n){for(int i=0;i<8;i++)p[i]=n>>(56-8*i);}
static void wr32be(uint8_t*p,uint32_t n){for(int i=0;i<4;i++)p[i]=n>>(24-8*i);}
static int efs_meta_apply_append_open(void*kv,uint64_t ino,uint64_t*off,uint64_t*len,uint32_t*n){(void)kv;(void)ino;assert(*n==1);*off=offset;*len=length;*n=open_count;return 0;}
static int efs_meta_append_completion_atomic_eligible(void*kv,uint64_t ino,uint64_t off){(void)kv;(void)ino;assert(off==offset);return eligible;}
static int efs_kv_key_append_rsv(uint32_t shard,uint64_t ino,uint64_t gen,uint64_t off,uint8_t*k,uint32_t*n){assert(shard==1&&ino==11&&gen==7&&off==offset);k[0]=1;*n=1;return 0;}
static int efs_kv_get(void*kv,const uint8_t*k,uint32_t n,uint8_t*v,uint32_t*vl){(void)kv;(void)k;assert(n==1);memcpy(v,reservation,48);*vl=stored_length;return lookup_rc;}
static int host_propose_mode(struct efs_raft_host*h,uint8_t group,const uint8_t*cmd,uint32_t n,uint64_t*index,uint64_t*term,int*hint,int local){(void)h;(void)hint;assert(group==0&&local&&n==EFS_MD_APPEND_COMPLETE_ATOMIC_LEN&&cmd[0]==EFS_MD_CMD_APPEND_COMPLETE_ATOMIC);assert(rd64be(cmd+1)==11&&rd64be(cmd+9)==7&&rd64be(cmd+17)==offset&&rd64be(cmd+45)==9);assert(!memcmp(cmd+53,reservation,48)&&cmd[25]==42&&rd32be(cmd+41)==3);submissions++;*index=19;*term=23;return submit_rc;}
static int host_wait_verdict(struct efs_raft_host*h,uint8_t group,uint64_t index,uint64_t term,int*hint){(void)h;(void)hint;assert(group==0&&index==19&&term==23);verdicts++;return verdict_rc;}
''' + s[a:b] + r'''
static void reset(void){raft=(struct efs_raft){2,7};eligible=1;lookup_rc=submit_rc=verdict_rc=0;submissions=verdicts=0;open_count=1;stored_length=48;offset=100;length=8;memset(reservation,0,48);wr64be(reservation,100);wr64be(reservation+8,8);reservation[16]=42;wr32be(reservation+32,3);}
int main(void){struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER};struct efs_meta_row row={7,9};struct efs_opid id={.client_uuid={42},.session_epoch=3};int hint=-1,handled;
 reset();assert(!host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)&&handled&&submissions==1&&verdicts==1);
 reset();assert(!host_complete_single_append(&h,11,107,&row,&id,&hint,&handled)&&!handled&&!submissions);
 reset();assert(!host_complete_single_append(&h,11,108,&row,NULL,&hint,&handled)&&!handled);
 reset();raft.role=0;assert(!host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)&&!handled);
 reset();raft.voters=1;assert(!host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)&&!handled);
 reset();eligible=0;assert(!host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)&&!handled);
 reset();eligible=EFS_ERR_BUSY;assert(host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)==EFS_ERR_BUSY&&!handled);
 reset();reservation[16]=43;assert(!host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)&&!handled);
 reset();open_count=0;assert(!host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)&&!handled);
 reset();length=0;assert(!host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)&&!handled);
 reset();offset=UINT64_MAX;assert(!host_complete_single_append(&h,11,UINT64_MAX,&row,&id,&hint,&handled)&&!handled);
 reset();wr64be(reservation+8,9);assert(host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)==EFS_ERR_BUSY&&!handled);
 reset();wr32be(reservation+32,4);assert(!host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)&&!handled);
 reset();stored_length=47;assert(host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)==EFS_ERR_PROTO&&!handled);
 reset();lookup_rc=EFS_ERR_NOT_FOUND;assert(host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)==EFS_ERR_BUSY&&!handled);
 reset();submit_rc=EFS_ERR_IO;assert(host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)==EFS_ERR_IO&&handled&&!verdicts);
 reset();verdict_rc=EFS_ERR_STALE;assert(host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)==EFS_ERR_BUSY&&handled&&verdicts==1);
 reset();verdict_rc=EFS_ERR_IO;assert(host_complete_single_append(&h,11,108,&row,&id,&hint,&handled)==EFS_ERR_IO&&handled);
 puts("single append host: published coverage, identity/FileID binding, local nonsolo authority, cross-shard fallback and exact settled verdict PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-single-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
