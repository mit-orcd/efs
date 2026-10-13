#!/usr/bin/env python3
"""Two fresh admissions precede either send; older rounds cannot cover them."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_read_index_pair(');b=s.index('\n}',a)+2
code=r'''
#include "efs/common.h"
#include "efs/network.h"
#include <stdint.h>
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#define EFS_RAFT_LEADER 2
struct efs_raft {int role,pending,fail,quorum;uint64_t round,term,commit;};
struct efs_raft_host {pthread_mutex_t mu;struct efs_raft core[3];int absent;};
struct host_read_admission {uint64_t want,round,term;};
static int begun,waits,scalar,kicks,published,wait_error;
static uint64_t expected[3];
static __thread uint64_t deadline;
uint64_t efs_net_deadline_ms(void){return deadline;}
void efs_net_set_deadline_ms(uint64_t value){deadline=value;}
static struct efs_raft *group_raft(struct efs_raft_host*h,uint8_t g){assert(g==0 || g==2);return h->absent==g+1?NULL:&h->core[g];}
static int efs_raft_role(struct efs_raft*r){return r->role;}
static uint64_t efs_raft_read_round(struct efs_raft*r){return r->round;}
static uint64_t efs_raft_term(struct efs_raft*r){return r->term;}
static uint64_t efs_raft_commit(struct efs_raft*r){return r->commit;}
static int efs_raft_read_pending(struct efs_raft*r){return r->pending;}
static int efs_raft_read_quorum(struct efs_raft*r){return r->quorum;}
static int efs_raft_read_begin(struct efs_raft*r){begun++;if(r->fail)return r->fail;r->round++;r->pending=1;r->quorum=0;return 0;}
static void host_publish_group(struct efs_raft_host*h,uint8_t g){(void)h;(void)g;published++;}
static void host_pump_kick(struct efs_raft_host*h){(void)h;kicks++;}
static int host_read_index(struct efs_raft_host*h,uint8_t g,int*hint){(void)hint;
 if(h->core[g].role!=2 && h->core[2-g].role==2)
  assert(h->core[2-g].pending && begun<=1); /* local probe sent before remote wait */
 __atomic_add_fetch(&scalar,1,__ATOMIC_RELAXED);return wait_error;}
static int host_read_index_admitted(struct efs_raft_host*h,uint8_t g,int*hint,const struct host_read_admission*a){
 (void)hint;assert(a->round==expected[g]+1 && a->term==h->core[g].term && a->want==h->core[g].commit);
 /* Both rounds must already be pending before the first wait. Their old
  * contexts are less than the captured minimum eligible round. */
 assert(h->core[g].pending);
 if(h->core[0].role==2 && h->core[2].role==2)
  assert(h->core[0].pending && h->core[2].pending);
 waits++;return wait_error;
}
''' + (root/'src/server/read_index_pair_remote.h').read_text() + s[a:b]+r'''
static void host_need_both(uint8_t groups[2]){groups[0]=0;groups[1]=2;}
static int host_hosts(struct efs_raft_host*h,uint8_t g){return h->absent!=g+1;}
static int host_pick_peer(struct efs_raft_host*h,const uint8_t*g,unsigned n,int avoid){(void)h;assert(g[0]==0 && g[1]==2 && n==2 && avoid==-1);return 7;}
''' + s[s.index('static int host_append_authority('):s.index('\n}',s.index('static int host_append_authority('))+2]+r'''
static void reset(struct efs_raft_host*h){
 h->absent=0;h->core[0]=(struct efs_raft){.role=2,.round=7,.term=11,.commit=40};
 h->core[2]=(struct efs_raft){.role=2,.round=17,.term=21,.commit=90};
 expected[0]=7;expected[2]=17;begun=waits=scalar=kicks=published=wait_error=0;
}
int main(void){struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER};int hint=-1;
 reset(&h);h.absent=3;
 assert(host_append_authority(&h,&hint)==EFS_ERR_NOT_PRIMARY && hint==7 && !begun && !scalar);
 reset(&h);assert(!host_append_authority(&h,&hint) && begun==2 && waits==2);
 reset(&h);assert(!host_read_index_pair(&h,0,2,&hint));assert(begun==2 && waits==2 && kicks==1 && published==2 && !scalar);
 expected[0]=h.core[0].round;expected[2]=h.core[2].round;
 h.core[0].pending=h.core[2].pending=0;
 assert(!host_read_index_pair(&h,0,2,&hint));assert(begun==4 && waits==4); /* new RPC needs newer rounds */
 reset(&h);h.core[0].pending=h.core[2].pending=1;
 assert(!host_read_index_pair(&h,0,2,&hint));assert(!begun && waits==2); /* waiters own next eligible rounds */
 reset(&h);h.core[0].pending=h.core[2].pending=1;h.core[0].quorum=h.core[2].quorum=1;
 assert(!host_read_index_pair(&h,0,2,&hint));assert(begun==2 && waits==2);
 assert(!h.core[0].quorum && !h.core[2].quorum); /* fresh probes, not old proof authority */
 reset(&h);h.core[2].role=0;h.core[0].pending=h.core[0].quorum=1;
 assert(!host_read_index_pair(&h,0,2,&hint) && begun==1 && waits==1 && scalar==1);
 reset(&h);h.core[2].pending=1;assert(!host_read_index_pair(&h,0,2,&hint));assert(begun==1 && waits==2);
 reset(&h);h.core[2].role=0;assert(!host_read_index_pair(&h,0,2,&hint));assert(scalar==1 && begun==1 && waits==1 && kicks==1);
 reset(&h);h.core[0].role=0;assert(!host_read_index_pair(&h,0,2,&hint));assert(scalar==1 && begun==1 && waits==1);
 reset(&h);h.core[2].role=0;h.core[0].pending=1;
 assert(!host_read_index_pair(&h,0,2,&hint) && !begun && scalar==1 && waits==1);
 assert(host_read_pair_count==0); /* mixed leadership never needs helper capacity */
 reset(&h);h.core[0].role=h.core[2].role=0;assert(!host_read_index_pair(&h,0,2,&hint));assert(scalar==2 && !begun && !waits);
 reset(&h);h.core[2].role=0;h.core[0].round=UINT64_MAX;
 assert(host_read_index_pair(&h,0,2,&hint)==EFS_ERR_BUSY && !begun && !scalar);
 reset(&h);h.core[2].role=0;h.core[0].fail=EFS_ERR_IO;
 assert(host_read_index_pair(&h,0,2,&hint)==EFS_ERR_IO && begun==1 && !scalar && !waits);
 reset(&h);h.core[2].role=0;wait_error=EFS_ERR_NOT_PRIMARY;
 assert(host_read_index_pair(&h,0,2,&hint)==EFS_ERR_NOT_PRIMARY && begun==1 && scalar==1 && !waits);
 reset(&h);h.absent=3;wait_error=EFS_ERR_NOT_PRIMARY;
 assert(host_read_index_pair(&h,0,2,&hint)==EFS_ERR_NOT_PRIMARY && scalar==1 && !begun);
 reset(&h);assert(!host_read_index_pair(&h,2,2,&hint));assert(scalar==1 && !begun);
 reset(&h);h.core[2].round=UINT64_MAX;
 assert(host_read_index_pair(&h,0,2,&hint)==EFS_ERR_BUSY && !begun && !waits);
 reset(&h);h.core[2].fail=EFS_ERR_IO;
 assert(host_read_index_pair(&h,0,2,&hint)==EFS_ERR_IO && begun==2 && !waits && kicks==1);
 reset(&h);wait_error=EFS_ERR_NOT_PRIMARY;
 assert(host_read_index_pair(&h,0,2,&hint)==EFS_ERR_NOT_PRIMARY && waits==1);
 assert(!pthread_mutex_trylock(&h.mu));pthread_mutex_unlock(&h.mu);pthread_mutex_destroy(&h.mu);
 puts("ReadIndex pair: pre-send fresh admissions, pending rounds, independent requests, routing, overflow and failures PASS");
}
'''
# The actual waiter consumes the captured admission rather than admitting
# after a concurrently sent probe; it retains term/quorum/apply predicates.
a=s.index('static int host_read_index_admitted(');b=s.index('\n}',a)
body=s[a:b]
assert 'round=admission->round;admitted_term=admission->term' in body
assert 'if (v.term != admitted_term)' in body
assert 'host_view_covers(&v, want, round, admitted_term)' in body
with tempfile.TemporaryDirectory(prefix='efs-read-pair-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
