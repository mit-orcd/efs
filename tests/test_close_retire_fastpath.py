#!/usr/bin/env python3
"""Actual GC close pass: atomic empty retirement, bounded busy cleanup and errors."""
from pathlib import Path
import os,subprocess,tempfile,shlex
root=Path(__file__).resolve().parents[1];s=Path(os.environ.get('EFS_CLOSE_GC_SOURCE',str(root/'src/server/raft_host.c'))).read_text();a=s.index('struct close_step_scan {');b=s.index('static void host_gc_close_pass(',a)
code=r'''
#include "efs/common.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct stats {uint64_t gc_scan_errors;};
struct efs_raft_host {void *kv;struct stats *s;uint32_t close_step_cursor_len[2];uint8_t close_step_cursor[2][11];int gc_running;};
static unsigned phases[8],proposals,append_calls;static int initial_rc,lock_rc,append_rc;
static int gc_gi(uint8_t group){return group==0?0:1;}
static uint64_t rd64be(const uint8_t*p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=v<<8|p[i];return v;}
static uint32_t rd32be(const uint8_t*p){uint32_t v=0;for(unsigned i=0;i<4;i++)v=v<<8|p[i];return v;}
static void wr64be(uint8_t*p,uint64_t v){for(unsigned i=8;i;i--){p[i-1]=v;v>>=8;}}
#define efs_kv_scan_from fake_kv_scan_from
static int fake_kv_scan_from(void *kv,const uint8_t*p,uint32_t pl,const uint8_t*c,uint32_t cl,int(*cb)(void*,const uint8_t*,uint32_t,const uint8_t*,uint32_t),void*ctx){
 (void)kv;(void)c;(void)cl;assert(pl==3&&p[2]==EFS_KV_KIND_CLOSE_STEP);
 uint8_t key[11]={0},value[EFS_CLOSE_STEP_VALUE]={0};key[2]=EFS_KV_KIND_CLOSE_STEP;wr64be(key+3,42);
 wr64be(value,1);value[27]=1;value[35]=1;assert(!cb(ctx,key,sizeof(key),value,sizeof(value)));return 0;
}
static int host_gc_propose(struct efs_raft_host*h,uint8_t g,const uint8_t*cmd,uint32_t n){
 (void)h;(void)g;assert(n==46&&cmd[0]==EFS_MD_CMD_CLOSE_STEP&&rd64be(cmd+1)==42);phases[proposals++]=cmd[45];
 if(cmd[45]==EFS_CLOSE_STEP_LOCKS)return lock_rc;
 return proposals==1?initial_rc:EFS_OK;
}
static int host_close_append_step(struct efs_raft_host*h,efs_ino_t ino,const uint8_t*identity){
 (void)h;assert(ino==42&&rd64be(identity)==1);append_calls++;return append_rc;
}
static void host_gc_close_retire_batch(struct efs_raft_host*h,uint8_t g,const efs_ino_t*ino,
 const uint8_t identities[8][EFS_CLOSE_STEP_VALUE],unsigned n,int*results){
 for(unsigned i=0;i<n;i++) {uint8_t cmd[46]={EFS_MD_CMD_CLOSE_STEP};wr64be(cmd+1,ino[i]);memcpy(cmd+9,identities[i],EFS_CLOSE_STEP_VALUE);cmd[45]=EFS_CLOSE_STEP_RETIRE;results[i]=host_gc_propose(h,g,cmd,sizeof(cmd));}
}
'''+s[a:b]+r'''
static void run(int first,int locks,int append){
 struct stats stats={0};struct efs_raft_host h={.s=&stats,.gc_running=1};
 proposals=append_calls=0;initial_rc=first;lock_rc=locks;append_rc=append;
 host_gc_close_step_pass(&h,0,1);assert(!stats.gc_scan_errors);
}
int main(void){
 run(EFS_OK,0,0);assert(proposals==1&&phases[0]==EFS_CLOSE_STEP_RETIRE&&!append_calls);
 run(EFS_ERR_BUSY,0,0);assert(proposals==3&&phases[0]==EFS_CLOSE_STEP_RETIRE&&phases[1]==EFS_CLOSE_STEP_LOCKS&&phases[2]==EFS_CLOSE_STEP_RETIRE&&append_calls==1);
 run(EFS_ERR_BUSY,EFS_ERR_BUSY,0);assert(proposals==2&&!append_calls);
 run(EFS_ERR_BUSY,0,EFS_ERR_BUSY);assert(proposals==2&&append_calls==1);
 run(EFS_ERR_STALE,0,0);assert(proposals==1&&!append_calls);
 run(EFS_ERR_IO,0,0);assert(proposals==1&&!append_calls);
 puts("close GC: empty files retire through existing atomic barriers in one proposal; BUSY follows bounded lock/append cleanup; stale/error identities are preserved PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-close-fast-') as td:
 p=Path(td)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
