#!/usr/bin/env python3
"""Actual apply dispatcher preserves REGISTER/ESTABLISH business verdicts."""
import os,shlex,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_SESSION_APPLY_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('static int apply_session_cmd(');b=s.index('\n}',a)+2
code=r'''
#include "efs/session.h"
#include "efs/protocol.h"
#include "efs/meta_apply.h"
#include "efs/lock.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#define HOST_SESS_HDR_LEN 18
#define HOST_SESS_CREATE_LEN 22
#define HOST_SESS_REG_LEN 26
#define HOST_SESS_LEASE_LEN 38
struct efs_raft_host {struct efs_kv *kv;void *s;pthread_mutex_t wait_mu;};
static uint32_t rd32be(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint64_t rd64be(const uint8_t *p){return (uint64_t)rd32be(p)<<32|rd32be(p+4);}
static void wr32be(uint8_t *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static int server_put_session_fence(void *s,const uint8_t *uuid,uint32_t epoch){(void)s;(void)uuid;(void)epoch;return EFS_ERR_INVAL;}
static void host_lock_wait_drop_session(struct efs_raft_host *h,const uint8_t *uuid,uint32_t epoch){(void)h;(void)uuid;(void)epoch;}
static void lock_wait_signal_locked(struct efs_raft_host *h,efs_ino_t ino){(void)h;(void)ino;}
static int host_lease_close_effects(struct efs_raft_host *h,efs_ino_t ino,uint64_t gen){(void)h;(void)ino;(void)gen;return EFS_ERR_INVAL;}
static int host_unpack_session_cleanup(const uint8_t *cmd,uint32_t len,uint32_t *shard,uint32_t *epoch,struct efs_session_cleanup_key *keys,uint32_t *n){(void)cmd;(void)len;(void)shard;(void)epoch;(void)keys;(void)n;return EFS_ERR_INVAL;}
static int host_unpack_lease_edge(const uint8_t *cmd,uint32_t len,struct efs_opid_req *q,efs_ino_t *ino,uint64_t *gen,unsigned *flags,uint64_t *token){(void)cmd;(void)len;(void)q;(void)ino;(void)gen;(void)flags;(void)token;return EFS_ERR_INVAL;}
'''+s[a:b]+r'''
int main(void) {
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);
 struct efs_raft_host h={.kv=kv,.wait_mu=PTHREAD_MUTEX_INITIALIZER};
 uint8_t cmd[26]={EFS_MD_CMD_SESSION,EFS_MD_SESS_REGISTER};cmd[2]=71;wr32be(cmd+18,1);wr32be(cmd+22,1);
 struct efs_session_rec owner;
 /* Real missing-owner apply must drive the host's CREATE fallback. */
 assert(apply_session_cmd(&h,cmd,26,1)==EFS_ERR_NOT_FOUND);
 assert(efs_session_get(kv,cmd+2,&owner)==EFS_ERR_NOT_FOUND);
 cmd[1]=EFS_MD_SESS_CREATE;assert(!apply_session_cmd(&h,cmd,22,3));
 cmd[1]=EFS_MD_SESS_REGISTER;assert(!apply_session_cmd(&h,cmd,26,4));
 assert(!efs_session_get(kv,cmd+2,&owner)&&efs_session_bit_get(owner.touched,1));
 cmd[1]=EFS_MD_SESS_ESTABLISH;assert(!apply_session_cmd(&h,cmd,26,5));
 assert(!efs_session_accept(kv,1,cmd+2,1));
 assert(!efs_session_fence_local(kv,1,cmd+2,2));
 assert(apply_session_cmd(&h,cmd,26,9)==EFS_ERR_STALE);
 wr32be(cmd+22,EFS_SESSION_BITS);assert(apply_session_cmd(&h,cmd,26,10)==EFS_ERR_INVAL);
 wr32be(cmd+22,1);
 assert(!efs_session_begin_fence(kv,cmd+2));
 cmd[1]=EFS_MD_SESS_REGISTER;wr32be(cmd+18,2);wr32be(cmd+22,2);
 assert(apply_session_cmd(&h,cmd,26,6)==EFS_ERR_BUSY);
 wr32be(cmd+18,1);assert(apply_session_cmd(&h,cmd,26,7)==EFS_ERR_STALE);
 wr32be(cmd+22,EFS_SESSION_BITS);assert(apply_session_cmd(&h,cmd,26,8)==EFS_ERR_INVAL);
 efs_kv_mem_free(kv);
 puts("session apply: real missing-owner bootstrap, participant admission and exact BUSY/STALE/INVAL refusal PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-session-apply-') as td:
 p=Path(td)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'libefs.a'),'-pthread','-lm','-ldl','-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=30)
