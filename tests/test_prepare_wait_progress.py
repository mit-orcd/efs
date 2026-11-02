#!/usr/bin/env python3
"""Batch PREPARE/RESOLVE waits use the existing bounded own-entry policy.

Execute the production waiter/dispatch wrappers with controlled apply progress
and the real apply_wait.h policy. Slow progress must finish without abort/re-
proposal; stalled or over-cap progress and unknown/failed verdicts still refuse.
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
import csrc
root=Path(__file__).resolve().parents[1]
s=csrc.module('raft_host','PREPARE_WAIT_SOURCE')
def fn(name):
    m=re.search(r'^static int '+name+r'\([^;]+?\)\n\{',s,re.M)
    assert m,name
    return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include "efs/protocol.h"
#include "apply_wait.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define HOST_READ_TRIES 80
#define HOST_TICK_US 5000
#define HOST_APPLY_WAIT_CAP_US 2000000ull
struct efs_raft_host {uint64_t now, applied[3];int moving, verdict;unsigned own,reads,checks;};
struct host_idx_ref {uint8_t group;uint64_t idx,term;};
static int host_wait_applied_mode(struct efs_raft_host*h,uint8_t g,uint64_t idx,int *hint,int own)
{
 struct efs_apply_wait w;(void)hint;
 if(own)h->own++;else h->reads++;
 efs_apply_wait_start(&w,h->now,h->applied[g],HOST_READ_TRIES*HOST_TICK_US,
  own?HOST_APPLY_WAIT_CAP_US:HOST_READ_TRIES*HOST_TICK_US);
 while(h->applied[g]<idx){
  h->now+=100000;if(h->moving)h->applied[g]++;
  if(efs_apply_wait_expired(&w,h->now,h->applied[g]))return EFS_ERR_BUSY;
 }
 return EFS_OK;
}
static int host_apply_rc(struct efs_raft_host*h,uint8_t group,uint64_t idx,uint64_t term)
{assert(h->applied[group]>=idx && term==7);h->checks++;return h->verdict;}
'''+fn('host_wait_applied')+fn('host_wait_applied_own')+fn('host_wait_refs')+r'''
int main(void){
 (void)host_wait_applied_own; /* negative control may still call the read wrapper */
 struct host_idx_ref refs[3]={{0,3,7},{0,12,7},{2,6,7}};
 struct efs_raft_host h={.moving=1};int hint=-1;
 /* One wait per group, for the maximum index; all exact verdicts checked. */
 assert(!host_wait_refs(&h,refs,3,&hint));
 assert(h.own==2 && !h.reads && h.checks==3 && h.now==1800000);
 h=(struct efs_raft_host){.moving=1};
 assert(host_wait_applied(&h,0,12,&hint)==EFS_ERR_BUSY && h.now==400000);
 h=(struct efs_raft_host){0};
 assert(host_wait_refs(&h,refs,3,&hint)==EFS_ERR_BUSY && h.now==400000 && !h.checks);
 h=(struct efs_raft_host){.moving=1};refs[0].idx=30;
 assert(host_wait_refs(&h,refs,3,&hint)==EFS_ERR_BUSY && h.now==2000000 && !h.checks);
 refs[0].idx=3;
 int refusals[]={EFS_ERR_BUSY,EFS_ERR_NOT_PRIMARY,EFS_ERR_STALE,EFS_ERR_IO};
 for(unsigned i=0;i<sizeof(refusals)/sizeof(refusals[0]);i++){
  h=(struct efs_raft_host){.moving=1,.verdict=refusals[i]};
  assert(host_wait_refs(&h,refs,3,&hint)==refusals[i] && h.checks==3);
 }
 puts("PREPARE/RESOLVE: existing own-entry progress grant, fixed 2s cap, 400ms no-progress/read refusal, max-index waits and exact verdicts PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-prepare-wait-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(shlex.split(os.getenv('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/server'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
