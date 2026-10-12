#!/usr/bin/env python3
"""A superseded quorum completes admitted waiters, never post-send arrivals."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
raft=(root/'src/raft/raft.c').read_text();host=(root/'src/server/raft_host.c').read_text()
def function(source,name):
 a=source.index('int '+name+'(');return source[a:source.index('\n}',a)+2]
a=host.index('struct host_view {');b=host.index('\n};',a)+3
c=raft.index('static void read_proof_forget(');helper=raft[c:raft.index('\n}',c)+2]
code=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define EFS_RAFT_LEADER 2
struct efs_raft {int storage_failed,role,quorum;uint64_t read_round,read_index,current_term;
 uint64_t read_proven_round,read_proven_index,read_proven_term;
 uint32_t read_proven_cfg_old,read_proven_cfg_new,log_old,log_new;};
static int efs_raft_read_quorum(const struct efs_raft *r){return r->quorum;}
'''+helper+function(raft,'efs_raft_read_proven')+host[a:b]+'\nstatic '+function(host,'host_view_covers')+r'''
static struct host_view view(struct efs_raft *r,uint64_t applied){
 struct host_view v={.has=1,.role=r->role,.term=r->current_term,.read_pending=1,
   .read_round=r->read_round,.read_index=r->read_index,.applied=applied};
 v.read_done=efs_raft_read_quorum(r);
 v.proven_done=efs_raft_read_proven(r,&v.proven_round,&v.proven_index);return v;
}
int main(void){
 struct efs_raft r={.role=2,.read_round=12,.read_index=60,.current_term=9,
   .read_proven_round=11,.read_proven_index=50,.read_proven_term=9,
   .read_proven_cfg_old=7,.log_old=7};
 struct host_view v=view(&r,50);
 assert(!v.read_done && v.read_pending); /* publication cannot use new raw pending round */
 assert(!(v.read_done && v.read_round>=12 && v.read_index>=60));
 assert(host_view_covers(&v,50,11,9)); /* admitted before round11 send */
 assert(!host_view_covers(&v,50,12,9)); /* admitted during round11 */
 assert(!host_view_covers(&v,51,11,9)); /* later committed bytes */
 assert(!host_view_covers(&v,50,11,8));
 v=view(&r,49);assert(!host_view_covers(&v,50,11,9)); /* own apply parked */
 r.quorum=1;v=view(&r,50);
 /* Newer completed quorum must not move an older admitted waiter's apply
  * target from50 to60 when its own valid quorum already applied at50. */
 assert(host_view_covers(&v,50,11,9));
 assert(!host_view_covers(&v,51,11,9) && !host_view_covers(&v,50,12,9));
 v=view(&r,59);assert(!host_view_covers(&v,60,12,9));
 v=view(&r,60);assert(host_view_covers(&v,60,12,9));
 v.has=0;assert(!host_view_covers(&v,60,12,9));v.has=1;
 assert(!host_view_covers(&v,60,13,9)); /* next arrival cannot reuse */
 r.quorum=0;r.current_term++;v=view(&r,60);assert(!v.proven_done);
 r.current_term--;r.log_new=14;v=view(&r,60);assert(!v.proven_done);
 r.log_new=0;r.log_old=14;v=view(&r,60);assert(!v.proven_done);
 r.log_old=7;r.role=1;v=view(&r,60);assert(!v.proven_done);
 r.role=2;r.storage_failed=1;v=view(&r,60);assert(!v.proven_done);
 r.storage_failed=0;read_proof_forget(&r);v=view(&r,60);assert(!v.proven_done);
 assert(!r.read_proven_index && !r.read_proven_term && !r.read_proven_cfg_old && !r.read_proven_cfg_new);
 puts("completed ReadIndex: eligible waiter, post-send/new-commit refusal, apply, term/config/role/storage fences PASS");
}
'''
# Existing publication current-round proof must remain separate from retained completion.
assert 'rdone = efs_raft_read_quorum(r)' in host
assert 'v->read_done = V_LOAD(g->v_read_done)' in host
assert 'v->proven_done = V_LOAD(g->v_proven_done)' in host
with tempfile.TemporaryDirectory(prefix='efs-read-completion-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
