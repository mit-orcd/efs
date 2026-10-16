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
struct efs_raft {int role,pending,fail,quorum;uint64_t round,term,commit,read_index;};
struct host_group {uint64_t read_admit_count,read_begin_admits,read_begin_round,read_begin_term;};
struct server {int unused;};
struct efs_raft_host {pthread_mutex_t mu;struct efs_raft core[3];struct host_group g[3];struct server *s;int absent;};
static struct host_group *group_slot(struct efs_raft_host *h,uint8_t group){return &h->g[group];}
struct host_read_admission {uint64_t want,round,term;};
struct host_view {int has,role;uint64_t commit,term;};
static int begun,waits,scalar,kicks,published,wait_error;
static uint64_t expected[3],expected_want[3];
static int late_commit,regressed,view_mode;
static __thread uint64_t deadline;
uint64_t efs_net_deadline_ms(void){return deadline;}
void efs_net_set_deadline_ms(uint64_t value){deadline=value;}
static struct efs_raft *group_raft(struct efs_raft_host*h,uint8_t g){assert(g==0 || g==2);return h->absent==g+1?NULL:&h->core[g];}
static int efs_raft_role(struct efs_raft*r){return r->role;}
static uint64_t efs_raft_read_round(struct efs_raft*r){return r->round;}
static uint64_t efs_raft_term(struct efs_raft*r){return r->term;}
static uint64_t efs_raft_commit(struct efs_raft*r){return r->commit;}
uint64_t efs_raft_read_index(struct efs_raft*r){return r->read_index;}
int efs_raft_read_proven(struct efs_raft*r,uint64_t*round,uint64_t*index){(void)r;(void)round;(void)index;return 0;}
static int efs_raft_read_pending(struct efs_raft*r){return r->pending;}
static int efs_raft_read_quorum(struct efs_raft*r){return r->quorum;}
static int efs_raft_read_begin(struct efs_raft*r){begun++;if(r->fail)return r->fail;r->read_index=r->commit;r->round++;r->pending=1;r->quorum=0;return 0;}
static int host_read_begin_tracked(struct efs_raft_host *h,uint8_t group,struct efs_raft *r);
static void host_view_get(struct efs_raft_host *h,uint8_t g,struct host_view *v){
 v->has=1;v->role=h->core[g].role;v->term=h->core[g].term;v->commit=h->core[g].commit;
 if(g==2 && late_commit){
  assert(!host_read_begin_tracked(h,0,&h->core[0]));assert(!host_read_begin_tracked(h,2,&h->core[2]));
  if(late_commit==2)h->core[0].quorum=h->core[2].quorum=1;
  h->core[0].commit+=10;h->core[2].commit+=10;late_commit=0;
 }
 if(view_mode==1)v->has=0;
 if(view_mode==2)v->role=0;
 if(view_mode==3)v->term--;
 if(g==2 && regressed){h->core[2].commit--;regressed=0;}
}
static void host_publish_group(struct efs_raft_host*h,uint8_t g){(void)h;(void)g;published++;}
static void host_pump_kick(struct efs_raft_host*h){(void)h;kicks++;}
static int host_read_index(struct efs_raft_host*h,uint8_t g,int*hint){(void)h;(void)g;(void)hint;__atomic_add_fetch(&scalar,1,__ATOMIC_RELAXED);return wait_error;}
static int host_read_index_admitted(struct efs_raft_host*h,uint8_t g,int*hint,const struct host_read_admission*a,uint64_t *proof,int proof_only){assert(!proof && !proof_only);
 (void)hint;assert(a->round==expected[g]+1 && a->term==h->core[g].term && a->want==expected_want[g]);
 /* Both rounds must already be pending before the first wait. Their old
  * contexts are less than the captured minimum eligible round. */
 assert(h->core[g].pending);
 if(h->core[0].role==2 && h->core[2].role==2)
  assert(h->core[0].pending && h->core[2].pending);
 waits++;return wait_error;
}
''' + s[s.index('static int host_read_probe_needed('):s.index('/* Fresh metadata reads share one authority',s.index('static int host_read_probe_needed('))] + s[s.index('static uint64_t host_read_register('):s.index('static int host_read_index_admitted(')] + (root/'src/server/read_index_pair_remote.h').read_text() + s[a:b]+r'''
static void host_need_both(uint8_t groups[2]){groups[0]=0;groups[1]=2;}
static int host_hosts(struct efs_raft_host*h,uint8_t g){return h->absent!=g+1;}
static int host_pick_peer(struct efs_raft_host*h,const uint8_t*g,unsigned n,int avoid){(void)h;assert(g[0]==0 && g[1]==2 && n==2 && avoid==-1);return 7;}
''' + s[s.index('static int host_append_authority('):s.index('\n}',s.index('static int host_append_authority('))+2]+r'''
static void reset(struct efs_raft_host*h){
 h->absent=0;h->core[0]=(struct efs_raft){.role=2,.round=7,.term=11,.commit=40,.read_index=40};
 h->core[2]=(struct efs_raft){.role=2,.round=17,.term=21,.commit=90,.read_index=90};
 expected[0]=7;expected[2]=17;expected_want[0]=40;expected_want[2]=90;late_commit=regressed=view_mode=0;begun=waits=scalar=kicks=published=wait_error=0;
}
int main(void){struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER};int hint=-1;
 struct server server={1};h.s=&server;reset(&h);late_commit=1;
 assert(!host_read_index_pair(&h,0,2,&hint) && waits==2 && begun==2);
 assert(h.core[0].commit==50 && h.core[2].commit==100);
 server.unused=0;reset(&h);late_commit=2;
 assert(!host_read_index_pair(&h,0,2,&hint) && waits==2 && begun==2); /* pre-admitted completed cohort owes no redundant probes */
 
 reset(&h);regressed=1;assert(host_read_index_pair(&h,0,2,&hint)==EFS_ERR_BUSY && !waits && !begun);
 for(int mode=1;mode<=3;mode++){
  reset(&h);view_mode=mode;late_commit=1;expected_want[0]=50;expected_want[2]=100;
  assert(!host_read_index_pair(&h,0,2,&hint) && waits==2 && begun==2);
 }
 h.s=NULL;
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
 sanitized=Path(d)/'sanitized'
 subprocess.run(['cc','-std=gnu11','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(root/'include'),str(p),'-o',str(sanitized)],check=True)
 subprocess.run([str(sanitized)],check=True)
 negative=Path(d)/'late-floor.c'
 negative.write_text(code.replace('want=admitted[i].commit;', 'want=efs_raft_commit(r[i]);'))
 subprocess.run(['cc','-std=gnu11','-pthread','-I'+str(root/'include'),str(negative),'-o',str(negative.with_suffix(''))],check=True)
 result=subprocess.run([str(negative.with_suffix(''))],capture_output=True)
 assert result.returncode != 0, 'late-floor negative unexpectedly passed'
 late_probe=Path(d)/'late-probe.c'
 assert 'efs_raft_read_round(r) >= admission->round &&' in code
 late_probe.write_text(code.replace('efs_raft_read_round(r) >= admission->round &&','1 &&'))
 subprocess.run(['cc','-std=gnu11','-pthread','-I'+str(root/'include'),str(late_probe),'-o',str(late_probe.with_suffix(''))],check=True)
 result=subprocess.run([str(late_probe.with_suffix(''))],capture_output=True)
 assert result.returncode != 0, 'late-admission probe negative unexpectedly passed'
 print('ReadIndex late-admission negative: an earlier completed round cannot suppress the required fresh probe PASS')
 print('ReadIndex paired floor: unknown/follower/term fallback, ASan/UBSan and late-floor negative PASS')
