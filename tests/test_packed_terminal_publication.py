#!/usr/bin/env python3
"""Actual host publication phase: packed terminal commit, legacy admission proof."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('            /* Packed objects already have a durable admission');b=s.index('\n        }\n        /* Even STALE',a)
code=r'''
#include "efs/protocol.h"
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
struct efs_raft_host {struct efs_kv *kv;};
static unsigned proposals,reads,accepts;static uint8_t tags[4];static int proposal_rc,admit_rc,admit_verdict=EFS_META_PUBLICATION_PENDING;
static int host_propose_wait(struct efs_raft_host *h,uint8_t group,uint8_t *cmd,uint32_t len,int *hint){(void)h;assert(group==0&&len==1&&hint);tags[proposals++]=*cmd;return proposal_rc;}
static int host_read_index(struct efs_raft_host *h,uint8_t group,int *hint){(void)h;(void)group;(void)hint;reads++;return admit_rc;}
static int efs_session_accept(struct efs_kv *kv,uint32_t shard,const uint8_t *uuid,uint32_t epoch){(void)kv;(void)shard;(void)uuid;(void)epoch;accepts++;return 0;}
int efs_meta_publication_result(struct efs_kv *kv,const struct efs_meta_pub *p,int *out){(void)kv;(void)p;*out=admit_verdict;return 0;}
static int phase(uint8_t profile,int query_only){struct efs_raft_host host={0},*h=&host;struct efs_meta_pub p={.coding_profile_id=profile};struct efs_msg_publication request={0},*req=&request;int rc=0,hint=-1,verdict=0;uint8_t group=0,cmd[1]={query_only==2?EFS_MD_CMD_PUBLICATION_RETIRE:EFS_MD_CMD_PUBLICATION};uint32_t len=1,shard=1;
''' +s[a:b]+r'''
 return rc;}
static void reset(void){proposals=reads=accepts=0;proposal_rc=admit_rc=0;admit_verdict=EFS_META_PUBLICATION_PENDING;}
int main(void){
 reset();assert(!phase(EFS_META_PROFILE_PACK_K2F1,0)&&proposals==1&&tags[0]==EFS_MD_CMD_PUBLICATION&&!reads&&!accepts);
 reset();assert(!phase(EFS_META_PROFILE_K2F1,0)&&proposals==2&&tags[0]==EFS_MD_CMD_PUBLICATION_ADMIT&&tags[1]==EFS_MD_CMD_PUBLICATION&&reads==1&&accepts==1);
 reset();admit_rc=EFS_ERR_BUSY;assert(phase(EFS_META_PROFILE_K2F1,0)==EFS_ERR_BUSY&&proposals==1&&!accepts);
 reset();proposal_rc=EFS_ERR_IO;assert(phase(EFS_META_PROFILE_PACK_K2F1,0)==EFS_ERR_IO&&proposals==1&&!reads&&!accepts);
 reset();assert(!phase(EFS_META_PROFILE_PACK_K2F1,2)&&proposals==1&&tags[0]==EFS_MD_CMD_PUBLICATION_RETIRE);
 puts("packed publication: one terminal proposal, unknown retains ownership, legacy fresh admission proof and unchanged retirement PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-terminal-') as td:
 p=Path(td)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
