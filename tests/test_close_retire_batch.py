#!/usr/bin/env python3
"""Actual close-retirement coordinator preserves each independent verdict."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static void host_gc_close_retire_batch(struct efs_raft_host *h,')
body=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/common.h"
#include "efs/session.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct stats {uint64_t gc_reap_errors;};
struct efs_raft_host {struct stats *s;};
struct host_idx_ref {uint8_t group;uint64_t idx,term;};
struct host_command_cohort {unsigned n;};
static unsigned adds,ends,waits;static int flush_rc,capture_rc,verdict[8];
static void wr64be(uint8_t*p,uint64_t n){for(int i=7;i>=0;i--){p[i]=n;n>>=8;}}
static void host_command_cohort_begin(struct host_command_cohort*c,struct efs_raft_host*h){(void)h;c->n=0;}
static int host_command_cohort_add(struct efs_raft_host*h,struct host_idx_ref*r,const uint8_t*cmd,uint32_t n,int*hint){(void)h;assert(n==46&&!hint&&cmd[0]==EFS_MD_CMD_CLOSE_STEP&&cmd[45]==EFS_CLOSE_STEP_RETIRE&&r->group==2);assert(cmd[8]==adds+1&&cmd[9]==42);r->idx=++adds;r->term=19;return capture_rc;}
static int host_command_cohort_end(struct host_command_cohort*c,int prior,int*hint){(void)c;assert(!hint);ends++;return prior?prior:flush_rc;}
static int host_wait_verdict(struct efs_raft_host*h,uint8_t group,uint64_t idx,uint64_t term,int*hint){(void)h;assert(ends==1&&group==2&&term==19&&!hint&&idx==waits+1);return verdict[waits++];}
''' + body + r'''
static void run(int failure){struct stats stats={0};struct efs_raft_host h={&stats};efs_ino_t ino[8];uint8_t ids[8][EFS_CLOSE_STEP_VALUE]={{0}};int results[8];adds=ends=waits=0;flush_rc=failure;for(int i=0;i<8;i++){ino[i]=i+1;ids[i][0]=42;}
 host_gc_close_retire_batch(&h,2,ino,ids,8,results);
 for(int i=0;i<8;i++)assert(results[i]==(failure?failure:verdict[i]));
 assert(ends==1&&waits==(failure?0:8));assert(stats.gc_reap_errors==(failure?8:2));}
int main(void){verdict[1]=EFS_ERR_BUSY;verdict[3]=EFS_ERR_STALE;verdict[7]=EFS_ERR_IO;run(0);run(EFS_ERR_IO);capture_rc=EFS_ERR_IO;run(EFS_ERR_IO);puts("close retirement batch: independent exact verdicts, BUSY preserved, failed capture/fsync never accepted PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-close-batch-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
