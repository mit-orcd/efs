#!/usr/bin/env python3
"""Concurrent lock-free admissions select only rounds whose probes follow arrival."""
import os,shlex,subprocess,tempfile
from pathlib import Path
import csrc
root=Path(__file__).resolve().parents[1]
s=csrc.module('read_proof')
a=s.index('static void host_read_note_begin(');b=s.index('static int host_read_index_admitted(',a)
code=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define EFS_OK 0
#define EFS_ERR_BUSY -11
#define EFS_ERR_NOT_PRIMARY -20
#define EFS_RAFT_LEADER 2
#define READ_PROOF_NGROUPS 1
#define ROUNDS (1u<<17)
#define READERS 4
#define PER_READER 20000
struct efs_raft {uint64_t round,term;int role,pending,quorum;};
struct group_views {int unused;};
struct efs_raft_host {pthread_mutex_t mu;};
struct efs_telemetry;
'''+csrc.hblock('read_proof','struct read_proof_slot {','\n};\n')+csrc.hblock('read_proof','struct read_proof {','\n};\n')+csrc.hblock('read_proof','static inline struct read_proof_slot *read_proof_slot_of(','\n}\n')+r'''
struct host_view {int has,role;uint64_t commit,term,read_round;};
struct host_read_admission {uint64_t want,round,term;};
static uint64_t efs_raft_read_round(struct efs_raft*r){return r->round;}
static int efs_raft_role(struct efs_raft*r){return r->role;}
static int efs_raft_read_pending(struct efs_raft*r){return r->pending;}
static int efs_raft_read_quorum(struct efs_raft*r){return r->quorum;}
/* One global event clock: arrivals and probe sends take stamps from it. */
static uint64_t clock_;static uint64_t sent_at[ROUNDS+3];
static int efs_raft_read_begin(struct efs_raft*r){
 if(r->round>=ROUNDS)return EFS_ERR_BUSY;
 r->round++;r->pending=1;r->quorum=0;
 __atomic_store_n(&sent_at[r->round],__atomic_add_fetch(&clock_,1,__ATOMIC_SEQ_CST),__ATOMIC_RELEASE);
 return 0;
}
static void read_proof_host_kick(struct efs_raft_host*h){(void)h;}
static void host_publish_view(struct group_views*gv,int i,struct efs_raft*r){(void)gv;(void)i;(void)r;}
'''+s[a:b]+r'''
static struct efs_raft_host H;static struct read_proof RP;static struct efs_raft core={.round=0,.term=5,.role=EFS_RAFT_LEADER};
static int stop;
static struct {uint64_t arrival,round;} seen[READERS][PER_READER];
static void *reader(void *arg){
 unsigned id=(unsigned)(uintptr_t)arg;struct host_view v={1,EFS_RAFT_LEADER,1,5,0};
 for(unsigned i=0;i<PER_READER;i++){
  uint64_t arrival=__atomic_add_fetch(&clock_,1,__ATOMIC_SEQ_CST);
  struct host_read_admission ad;assert(!host_read_admit_view(&RP,0,&v,&ad));
  host_read_request(&RP,0,ad.round);
  seen[id][i].arrival=arrival;seen[id][i].round=ad.round;
 }
 return NULL;
}
static void *pump(void *arg){
 (void)arg;
 while(!__atomic_load_n(&stop,__ATOMIC_ACQUIRE)){
  pthread_mutex_lock(&H.mu);
  core.quorum=1;struct efs_raft *cores[1]={&core};(void)read_proof_start_requested(&RP,cores);
  pthread_mutex_unlock(&H.mu);
 }
 return NULL;
}
int main(void){
 pthread_mutex_init(&H.mu,NULL);RP.rp_host=&H;RP.rp_slot[0].rp_group=0;
 pthread_t p,t[READERS];assert(!pthread_create(&p,NULL,pump,NULL));
 for(uintptr_t i=0;i<READERS;i++)assert(!pthread_create(&t[i],NULL,reader,(void*)i));
 for(unsigned i=0;i<READERS;i++)assert(!pthread_join(t[i],NULL));
 __atomic_store_n(&stop,1,__ATOMIC_RELEASE);assert(!pthread_join(p,NULL));
 unsigned covered=0,shared=0;uint64_t last=0;
 for(unsigned i=0;i<READERS;i++)for(unsigned j=0;j<PER_READER;j++){
  uint64_t r=seen[i][j].round;assert(r>=1 && r<=ROUNDS+2); /* a refused begin at the cap still raises ri_begun */
  if(r<=core.round){covered++;assert(sent_at[r]>seen[i][j].arrival);} /* probes follow arrival */
  if(r==last)shared++;
  last=r;
 }
 assert(covered>0 && core.round<=__atomic_load_n(&RP.rp_slot[0].ri_request,__ATOMIC_ACQUIRE));
 assert(RP.rp_slot[0].ri_begun>=core.round && RP.rp_slot[0].ri_begun<=core.round+1);
 printf("raft read admission: %u admissions, %llu rounds, %u covered by later probes PASS\n",
        READERS*PER_READER,(unsigned long long)core.round,covered);
 (void)shared;pthread_mutex_destroy(&H.mu);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-admit-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',*shlex.split(os.environ.get('EFS_TEST_CFLAGS','')),'-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=60)
# The direct (!h->s) mode is gone: readers never take h->mu to admit or begin, there is
# no locked scalar admission and no locked begin in the waiter. Final authority
# predicates are unchanged.
a=s.index('static int host_read_index_admitted(');body=s[a:s.index('\n}',a)]
assert 'host_read_admit_view(' in body and 'pthread_mutex_lock' not in body and 'delegated' not in body
assert 'host_read_admit(' not in s and 'host_read_probe_needed' not in s
assert 'host_view_covers(&v, want, round, admitted_term)' in body
assert 'if (v.term != admitted_term)' in body
a=s.index('int read_proof_read_pair(struct read_proof *rp,uint8_t first,');pair=s[a:s.index('\n}',a)]
assert 'host_read_admit_view(' in pair and 'pthread_mutex_lock' not in pair and 'h->s' not in pair
a=s.index('static void host_read_note_begin(');begin=s[a:s.index('static int host_read_admit_view(',a)]
assert begin.index('host_read_note_begin(rp,group,r);')<begin.index('return efs_raft_read_begin(r);')
print('raft read admission: no h->mu on the reader path and no direct mode; ri_begun raised before read_begin PASS')
