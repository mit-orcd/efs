#!/usr/bin/env python3
"""Production fresh owner/floor proof before reusing durable establishment."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
code=r'''
#include "efs/session.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
struct efs_raft_host {struct efs_kv *kv;};
static int owner_rc,read_rc,local_rc,owner_calls,read_calls,local_calls;
static uint32_t rd32be(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int host_session_establish_admit(struct efs_raft_host *h,const uint8_t *cmd,uint32_t n,uint8_t group,int *hint){assert(h&&cmd&&n==26&&group==0&&hint);owner_calls++;return owner_rc;}
static int host_read_index(struct efs_raft_host *h,uint8_t group,int *hint){assert(h&&group==0&&hint);assert(owner_calls==1);read_calls++;return read_rc;}
int efs_session_accept(struct efs_kv *kv,uint32_t shard,const uint8_t uuid[16],uint32_t epoch){assert(kv&&shard==17&&uuid[0]==9&&epoch==1);assert(read_calls==1);local_calls++;return local_rc;}
#include "publication_session_fast.h"
static void reset(void){owner_rc=read_rc=local_rc=owner_calls=read_calls=local_calls=0;}
int main(void){
 struct efs_kv kv={0};struct efs_raft_host h={.kv=&kv};int hint=-1,reused=99;uint8_t cmd[26]={0};cmd[2]=9;cmd[21]=1;cmd[25]=17;
 reset();assert(!host_session_establish_reuse(&h,cmd,26,0,&hint,&reused)&&reused==1&&owner_calls==1&&read_calls==1&&local_calls==1);
 /* K5: only UNREGISTERED (no established row yet) falls back to ESTABLISH. */
 reset();local_rc=EFS_ERR_UNREGISTERED;assert(!host_session_establish_reuse(&h,cmd,26,0,&hint,&reused)&&!reused&&local_calls==1);
 reset();local_rc=EFS_ERR_BUSY;assert(host_session_establish_reuse(&h,cmd,26,0,&hint,&reused)==EFS_ERR_BUSY&&!reused);
 reset();local_rc=EFS_ERR_FENCED;assert(host_session_establish_reuse(&h,cmd,26,0,&hint,&reused)==EFS_ERR_FENCED&&!reused);
 reset();local_rc=EFS_ERR_IO;assert(host_session_establish_reuse(&h,cmd,26,0,&hint,&reused)==EFS_ERR_IO&&!reused);
 reset();read_rc=EFS_ERR_BUSY;assert(host_session_establish_reuse(&h,cmd,26,0,&hint,&reused)==EFS_ERR_BUSY&&!reused&&!local_calls);
 reset();read_rc=EFS_ERR_IO;assert(host_session_establish_reuse(&h,cmd,26,0,&hint,&reused)==EFS_ERR_IO&&!reused&&!local_calls);
 reset();owner_rc=EFS_ERR_STALE;assert(host_session_establish_reuse(&h,cmd,26,0,&hint,&reused)==EFS_ERR_STALE&&!reused&&!read_calls&&!local_calls);
 reset();owner_rc=EFS_ERR_BUSY;assert(host_session_establish_reuse(&h,cmd,26,0,&hint,&reused)==EFS_ERR_BUSY&&!reused&&!read_calls&&!local_calls);
 /* A new RPC always checks both authorities; prior success is no cache. */
 reset();assert(!host_session_establish_reuse(&h,cmd,26,0,&hint,&reused)&&reused);
 reset();local_rc=EFS_ERR_FENCED;assert(host_session_establish_reuse(&h,cmd,26,0,&hint,&reused)==EFS_ERR_FENCED&&!reused);
 puts("publication session reuse: fresh owner and local floor, missing establishment fallback, revocation/read/IO refusal and next-RPC recheck PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-session-reuse-') as d:
 p=Path(d)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/server'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
