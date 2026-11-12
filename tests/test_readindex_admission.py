#!/usr/bin/env python3
"""Scalar ReadIndex admission as the server runs it: the reader admits itself from
the published view and ri_begun, raises ri_request, and the pump starts the round.
Arrivals during an unfinished round queue for the next one and never borrow it."""
from pathlib import Path
import os, shlex, subprocess, tempfile
import csrc
root=Path(__file__).resolve().parents[1]
s=csrc.module('read_proof')
a=s.index('static void host_read_note_begin(');b=s.index('static int host_read_index_admitted(',a)
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define EFS_RAFT_LEADER 2
#define READ_PROOF_NGROUPS 2
struct efs_raft {int role,pending,fail,quorum;uint64_t round;};
struct group_views {int unused;};
struct efs_raft_host {int unused;};
struct efs_telemetry;
'''+csrc.hblock('read_proof','struct read_proof_slot {','\n};\n')+csrc.hblock('read_proof','struct read_proof {','\n};\n')+csrc.hblock('read_proof','static inline struct read_proof_slot *read_proof_slot_of(','\n}\n')+r'''
struct host_view {int has,role;uint64_t commit,term,read_round;};
struct host_read_admission {uint64_t want,round,term;};
static unsigned starts,published,kicks;
static int efs_raft_role(struct efs_raft*r){return r->role;}
static uint64_t efs_raft_read_round(struct efs_raft*r){return r->round;}
static int efs_raft_read_pending(struct efs_raft*r){return r->pending;}
static int efs_raft_read_quorum(struct efs_raft*r){return r->quorum;}
static int efs_raft_read_begin(struct efs_raft*r){starts++;if(r->fail)return r->fail;r->round++;r->pending=1;r->quorum=0;return EFS_OK;}
static void read_proof_host_kick(struct efs_raft_host*h){(void)h;kicks++;}
static void host_publish_view(struct group_views*gv,int i,struct efs_raft*r){(void)gv;(void)i;(void)r;published++;}
'''+s[a:b]+r'''
int main(void){
 struct efs_raft core={.round=10,.role=EFS_RAFT_LEADER};struct efs_raft *cores[2]={&core,NULL};
 struct efs_raft_host host={0};struct read_proof h={.rp_host=&host,.rp_slot={{.rp_group=0},{.rp_group=2}}};
 struct host_view v={1,EFS_RAFT_LEADER,40,7,10};struct host_read_admission ad;
 /* A reader admits from the view: commit floor, the next round after ri_begun, the term;
  * it begins nothing itself, it only raises the request. */
 h.rp_slot[0].ri_begun=10;
 assert(!host_read_admit_view(&h,0,&v,&ad) && ad.want==40 && ad.round==11 && ad.term==7);
 host_read_request(&h,0,ad.round);
 assert(starts==0 && h.rp_slot[0].ri_request==11 && kicks==1);
 /* The pump starts it once for every queued reader. */
 assert(read_proof_start_requested(&h,cores)==1u && starts==1 && core.round==11 && core.pending && published==1);
 assert(h.rp_slot[0].ri_begun==11);
 /* Arrivals while round 11 has no quorum wait for round 12: commits differ, the round is shared,
  * and the pump starts nothing while the previous round is unproven. */
 for(unsigned i=0;i<100;i++){
  v.commit=41+i;v.read_round=11;
  assert(!host_read_admit_view(&h,0,&v,&ad) && ad.round==12 && ad.want==41+i && ad.term==7);
  host_read_request(&h,0,ad.round);
  assert(!read_proof_start_requested(&h,cores) && starts==1);
 }
 assert(kicks==2); /* one kick for the raise to 12, shared by the 100 readers */
 /* Quorum 11 lands: the pump starts round 12 and later arrivals need 13. */
 core.quorum=1;v.commit=200;
 assert(read_proof_start_requested(&h,cores)==1u && starts==2 && core.round==12 && !core.quorum);
 assert(!host_read_admit_view(&h,0,&v,&ad) && ad.round==13 && ad.want==200);
 /* A completed quorum parked on own apply does not cover a fresh arrival: it asks for the next round. */
 core.pending=1;core.quorum=1;v.read_round=12;
 assert(!host_read_admit_view(&h,0,&v,&ad) && ad.round==13);
 host_read_request(&h,0,13);
 assert(read_proof_start_requested(&h,cores)==1u && core.round==13 && !core.quorum);
 /* A failed begin starts nothing and publishes nothing; the pump retries on its next cycle. */
 published=0;starts=0;core.quorum=1;core.fail=EFS_ERR_IO;host_read_request(&h,0,14);
 assert(!read_proof_start_requested(&h,cores) && starts==1 && published==0 && core.round==13);
 core.fail=0;assert(read_proof_start_requested(&h,cores)==1u && core.round==14);
 /* Refusals: not the leader, no replica, unknown group, saturation. */
 v.role=1;assert(host_read_admit_view(&h,0,&v,&ad)==EFS_ERR_NOT_PRIMARY);v.role=EFS_RAFT_LEADER;
 v.has=0;assert(host_read_admit_view(&h,0,&v,&ad)==EFS_ERR_NOT_PRIMARY);v.has=1;
 assert(host_read_admit_view(&h,1,&v,&ad)==EFS_ERR_NOT_PRIMARY);
 h.rp_slot[0].ri_begun=UINT64_MAX-1;assert(host_read_admit_view(&h,0,&v,&ad)==EFS_ERR_BUSY);
 h.rp_slot[0].ri_begun=3;v.read_round=UINT64_MAX-1;assert(host_read_admit_view(&h,0,&v,&ad)==EFS_ERR_BUSY);
 puts("scalar ReadIndex admission: view-based floor, shared next round, pump-started rounds, failed begin, refusal and overflow PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-admission-') as td:
 p=Path(td)/'test.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
