#!/usr/bin/env python3
"""Production publication errors and WAL failures cannot advance checkpoints."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int apply_publish_cmd(');b=s.index('\n/* Same encoding as sim_txn_apply.',a)
c=s.index('static int host_persist_durable_tail(');d=s.index('\nstatic int load_applied(',c)
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <stdint.h>
#define HOST_PUBLISH_LEN 202u
#define HOST_NGROUPS 2
#define APPLY_LOG(...) ((void)0)
struct efs_kv;
struct efs_raft_host {struct efs_kv *kv;};
static unsigned calls,holds,releases,persists;static int hold_rc,release_rc,results[3],persist_rc;
static int raft_dbg_on(void){return 0;}
static int apply_one_publish(struct efs_raft_host*h,const uint8_t*cmd,uint64_t i,uint64_t t,uint8_t g){(void)h;(void)cmd;(void)i;(void)t;(void)g;return results[calls++%3];}
static int efs_kv_lsm_sync_hold(struct efs_kv*kv){(void)kv;holds++;return hold_rc;}
static int efs_kv_lsm_sync_release(struct efs_kv*kv){(void)kv;releases++;return release_rc;}
static int persist_applied(struct efs_raft_host*h,int group,uint64_t index,uint64_t now,int force){(void)h;assert(index==10u+(unsigned)group && now==42 && (force==0 || force==1));persists++;return group?EFS_OK:persist_rc;}
''' +s[a:b]+s[c:d]+r'''
int main(void){struct efs_raft_host h={0};uint8_t commands[606]={0};uint64_t indices[]={10,11};
 results[0]=EFS_ERR_STALE;results[1]=EFS_ERR_IO;
 assert(apply_publish_cmd(&h,commands,sizeof(commands),1,1,0)==EFS_ERR_IO && calls==2 && holds==1 && releases==1);
 calls=0;hold_rc=EFS_ERR_IO;
 assert(apply_publish_cmd(&h,commands,sizeof(commands),1,1,0)==EFS_ERR_IO && !calls && releases==1);
 hold_rc=0;results[0]=results[1]=results[2]=EFS_OK;release_rc=EFS_ERR_IO;
 assert(apply_publish_cmd(&h,commands,sizeof(commands),1,1,0)==EFS_ERR_IO && calls==3);
 assert(host_persist_durable_tail(&h,indices,42,0)==EFS_ERR_IO && !persists);
 assert(host_persist_durable_tail(&h,indices,42,1)==EFS_ERR_IO && !persists);
 release_rc=0;assert(!host_persist_durable_tail(&h,indices,42,0) && persists==2);
 persist_rc=EFS_ERR_IO;assert(host_persist_durable_tail(&h,indices,42,1)==EFS_ERR_IO && persists==4);
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-publication-persistence-') as directory:
 p=Path(directory)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('publication persistence: IO overrides STALE, hold/release failures and periodic/forced checkpoint refusal PASS')
