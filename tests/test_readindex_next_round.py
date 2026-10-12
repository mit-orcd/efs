#!/usr/bin/env python3
"""Pump-owned next-round demand never resets a pending proof or grants authority."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static void host_request_next_read(');b=s.index('static int host_read_index_admitted(',a)
code=r'''
#include "efs/common.h"
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define EFS_RAFT_LEADER 2
struct efs_raft {int role,pending,fail,begun;uint64_t round,term,commit,probe_index,acks;};
struct host_group {struct efs_raft *r;uint64_t read_requested_round,read_requested_term;};
static int efs_raft_role(struct efs_raft *r){return r->role;}
static int efs_raft_read_pending(struct efs_raft *r){return r->pending;}
static uint64_t efs_raft_term(struct efs_raft *r){return r->term;}
static uint64_t efs_raft_read_round(struct efs_raft *r){return r->round;}
static int efs_raft_read_begin(struct efs_raft *r){
 assert(!r->pending);r->begun++;
 if(r->fail)return r->fail;
 r->round++;r->pending=1;r->probe_index=r->commit;r->acks=1;return 0;
}
''' + s[a:b] + r'''
int main(void){
 struct efs_raft r={.role=2,.pending=1,.round=10,.term=9,.commit=50,.acks=7};
 struct host_group g={.r=&r};
 for(unsigned i=0;i<112;i++)host_request_next_read(&g,&r,11);
 assert(g.read_requested_round==11 && g.read_requested_term==9);
 for(unsigned i=0;i<100;i++)assert(!host_start_requested_read(&g));
 assert(!r.begun && r.round==10 && r.acks==7); /* pending quorum untouched */
 r.pending=0;r.commit=60;
 assert(!host_start_requested_read(&g) && r.begun==1 && r.round==11 && r.probe_index==60);
 assert(!g.read_requested_round && !g.read_requested_term && r.acks==1);
 for(unsigned i=0;i<100;i++)assert(!host_start_requested_read(&g));
 assert(r.begun==1); /* no continuous probe loop when no reader is queued */
 host_request_next_read(&g,&r,12);r.round=12;
 assert(!host_start_requested_read(&g) && !g.read_requested_round && r.begun==1); /* another starter */
 host_request_next_read(&g,&r,13);r.term++;
 assert(!host_start_requested_read(&g) && !g.read_requested_round && r.begun==1); /* term invalidation */
 host_request_next_read(&g,&r,13);r.role=1;
 assert(!host_start_requested_read(&g) && !g.read_requested_round && r.begun==1);
 r.role=2;host_request_next_read(&g,&r,13);g.r=NULL;
 assert(!host_start_requested_read(&g) && !g.read_requested_round);g.r=&r;
 host_request_next_read(&g,&r,13);r.pending=0;r.fail=EFS_ERR_IO;
 assert(host_start_requested_read(&g)==EFS_ERR_IO && g.read_requested_round==13);
 r.fail=0;assert(!host_start_requested_read(&g) && r.round==13 && !g.read_requested_round);
 r.pending=0;host_request_next_read(&g,&r,14);assert(!g.read_requested_round); /* scalar starter owns idle round */
 puts("next ReadIndex round: bounded cohort, pending ACKs, fresh prefix, no spin, term/role/removal and retry PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-next-') as d:
    p=Path(d)/'t.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
