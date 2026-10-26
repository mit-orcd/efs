#!/usr/bin/env python3
"""Production publication errors and WAL failures cannot advance checkpoints."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static void unpack_report_ticket(');b=s.index('\n/* Same encoding as sim_txn_apply.',a)
c=s.index('static int host_save_applied(');d=s.index('\nstatic int load_applied(',c)
code=r'''
#include "efs/protocol.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define HOST_PUBLISH_LEN 202u
#define HOST_REPORT_PREFIX_LEN 21u
#define HOST_PUB_F_LANE_LOCAL 1
#define HOST_PUB_F_FRESH_OBJECT 2
#define HOST_PUB_F_TICKETED 4
#define HOST_PUB_F_UNCONDITIONAL 8
#define HOST_PUB_F_MISSING_SHIFT 4
#define HOST_PUB_F_MISSING_MASK (7u << HOST_PUB_F_MISSING_SHIFT)
#define HOST_PUB_F_REJECT_GONE 128
#define HOST_REPORT_REC_LEN (HOST_PUBLISH_LEN + 16u)
#define HOST_NGROUPS 2
#define APPLY_LOG(...) ((void)0)
struct efs_raft_host {struct efs_kv *kv;};
static unsigned calls,holds,releases,persists;static int hold_rc,release_rc,results[3],persist_rc;
static uint32_t rd32be(const uint8_t*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t rd64be(const uint8_t*p){return ((uint64_t)rd32be(p)<<32)|rd32be(p+4);}
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;return lane;}
uint8_t efs_raft_shard_group(uint32_t shard){(void)shard;return 0;}
int efs_session_accept(struct efs_kv*kv,uint32_t shard,const uint8_t*uuid,uint32_t epoch){(void)kv;(void)shard;(void)uuid;(void)epoch;return EFS_OK;}
static void unpack_publication(const uint8_t*p,struct efs_meta_pub*out,uint64_t idx,uint64_t term,uint8_t group){(void)p;(void)idx;(void)term;(void)group;memset(out,0,sizeof(*out));}
int efs_meta_apply_publish(struct efs_kv*kv,const struct efs_meta_pub*p){(void)kv;(void)p;return results[calls++%3];}
int efs_meta_apply_publish_reject(struct efs_kv*kv,const struct efs_meta_pub*p,int v){(void)kv;(void)p;(void)v;assert(0);return EFS_OK;}
static int efs_kv_lsm_sync_hold(struct efs_kv*kv){(void)kv;holds++;return hold_rc;}
static int efs_kv_lsm_sync_release(struct efs_kv*kv){(void)kv;releases++;return release_rc;}
static int persist_applied(struct efs_raft_host*h,int group,uint64_t index,uint64_t now,int force){(void)h;assert(index==10u+(unsigned)group && now==42 && (force==0 || force==1));persists++;return group?EFS_OK:persist_rc;}
''' +s[a:b]+s[c:d]+r'''
int main(void){struct efs_raft_host h={0};uint8_t commands[HOST_REPORT_PREFIX_LEN+3*HOST_REPORT_REC_LEN]={EFS_MD_CMD_REPORT_PUBLISH};uint64_t indices[]={10,11};
 struct efs_opid id={.session_epoch=1};id.client_uuid[0]=9;efs_report_session_pack(commands+1,&id);
 for(unsigned r=HOST_REPORT_PREFIX_LEN;r<sizeof(commands);r+=HOST_REPORT_REC_LEN){commands[r]=EFS_MD_CMD_PUBLISH;commands[r+HOST_PUBLISH_LEN-1]=HOST_PUB_F_TICKETED|HOST_PUB_F_FRESH_OBJECT;}
 results[0]=EFS_ERR_STALE;results[1]=EFS_ERR_IO;
 assert(apply_report_publish_cmd(&h,commands,sizeof(commands),0)==EFS_ERR_IO && calls==2 && holds==1 && releases==1);
 calls=0;hold_rc=EFS_ERR_IO;
 assert(apply_report_publish_cmd(&h,commands,sizeof(commands),0)==EFS_ERR_IO && !calls && releases==1);
 hold_rc=0;results[0]=results[1]=results[2]=EFS_OK;release_rc=EFS_ERR_IO;
 assert(apply_report_publish_cmd(&h,commands,sizeof(commands),0)==EFS_ERR_IO && calls==3);
 assert(host_persist_durable_tail(&h,indices,42,0)==EFS_ERR_IO && !persists);
 assert(host_persist_durable_tail(&h,indices,42,1)==EFS_ERR_IO && !persists);
 release_rc=0;assert(!host_persist_durable_tail(&h,indices,42,0) && persists==2);
 persist_rc=EFS_ERR_IO;assert(host_persist_durable_tail(&h,indices,42,1)==EFS_ERR_IO && persists==3);
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-publication-persistence-') as directory:
 p=Path(directory)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('publication persistence (ticketed REPORT entries): IO overrides STALE, hold/release failures and periodic/forced checkpoint refusal PASS')
