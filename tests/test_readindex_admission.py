#!/usr/bin/env python3
"""Actual scalar admission: start once, never cover post-send arrivals."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_read_admit(');b=s.index('\n}',a)+2
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define EFS_RAFT_LEADER 2
struct efs_raft {int role,pending,error;uint64_t round,commit,term;};
struct host_read_admission {uint64_t want,round,term;};
static unsigned starts;
static int efs_raft_role(struct efs_raft*r){return r->role;}
static uint64_t efs_raft_read_round(struct efs_raft*r){return r->round;}
static uint64_t efs_raft_commit(struct efs_raft*r){return r->commit;}
static uint64_t efs_raft_term(struct efs_raft*r){return r->term;}
static int efs_raft_read_pending(struct efs_raft*r){return r->pending;}
static int efs_raft_read_begin(struct efs_raft*r){starts++;if(r->error)return r->error;r->round++;r->pending=1;return EFS_OK;}
'''+s[a:b]+r'''
int main(void){
 struct efs_raft r={.role=2,.round=10,.commit=40,.term=7};
 struct host_read_admission a;int started=99;
 assert(!host_read_admit(&r,&a,&started));
 assert(started && starts==1 && r.round==11 && r.pending);
 assert(a.want==40 && a.round==11 && a.term==7);
 /* These arrivals must wait for round12; quorum11 cannot cover them. */
 for(unsigned i=0;i<100;i++){
  r.commit=41+i;assert(!host_read_admit(&r,&a,&started));
  assert(!started && starts==1 && a.round==12 && a.want==41+i && a.term==7);
 }
 r.pending=0;r.commit=200;
 assert(!host_read_admit(&r,&a,&started) && started && starts==2);
 assert(a.round==12 && a.want==200 && r.round==12);
 r.pending=0;r.error=EFS_ERR_IO;
 assert(host_read_admit(&r,&a,&started)==EFS_ERR_IO && !started && starts==3);
 r.error=0;r.round=UINT64_MAX;
 assert(host_read_admit(&r,&a,&started)==EFS_ERR_BUSY && !started && starts==3);
 r.role=1;r.round=20;
 assert(host_read_admit(&r,&a,&started)==EFS_ERR_NOT_PRIMARY && !started && starts==3);
 assert(host_read_admit(NULL,&a,&started)==EFS_ERR_NOT_PRIMARY && !started);
 puts("scalar ReadIndex admission: atomic fresh start, pending floor, fresh commit, refusal and overflow PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-admission-') as td:
 p=Path(td)/'test.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
