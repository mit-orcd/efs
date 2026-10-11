#!/usr/bin/env python3
"""Actual packed lifecycle route: followers cannot enter admission/forwarding."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('        /* Like policy/session commands, packed lifecycle RPCs');b=s.index('        uint32_t shard = group ==',a)
code=r'''
#include "efs/protocol.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
struct host_view {int has,role,leader;};
static struct host_view current;
struct efs_raft_host {int unused;};
static int admissions;
static void host_view_get(struct efs_raft_host *h,uint8_t group,struct host_view *out){(void)h;assert(group==0||group==2);*out=current;}
static int host_pick_peer(struct efs_raft_host *h,const uint8_t *group,int count,int skip){(void)h;assert((*group==0||*group==2)&&count==1&&skip==-1);return 2;}
static void route(struct efs_raft_host *h,uint8_t group,struct efs_msg_raft_mkfs_reply *out){
''' +s[a:b]+r'''
 admissions++;out->rc=0;
}
int main(void){struct efs_raft_host h={0};struct efs_msg_raft_mkfs_reply r;
 current=(struct host_view){1,EFS_RAFT_FOLLOWER,1};route(&h,0,&r);assert(r.rc==EFS_ERR_NOT_PRIMARY&&r.leader_hint==1&&!admissions);
 current=(struct host_view){1,EFS_RAFT_CANDIDATE,-1};route(&h,2,&r);assert(r.rc==EFS_ERR_NOT_PRIMARY&&r.leader_hint==-1&&!admissions);
 current=(struct host_view){0,0,-1};route(&h,2,&r);assert(r.rc==EFS_ERR_NOT_PRIMARY&&r.leader_hint==2&&!admissions);
 current=(struct host_view){1,EFS_RAFT_LEADER,1};route(&h,0,&r);assert(!r.rc&&admissions==1);
 current.role=EFS_RAFT_FOLLOWER;current.leader=2;route(&h,0,&r);assert(r.rc==EFS_ERR_NOT_PRIMARY&&r.leader_hint==2&&admissions==1);
 puts("packed lifecycle: follower/unhosted/election hints without admission or forwarding; leader-only admission and role-change redirect PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-route-') as td:
 p=Path(td)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
