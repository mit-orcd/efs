#!/usr/bin/env python3
"""Actual pre-mutex tickets select only rounds whose probes follow admission."""
import os,shlex,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('static uint64_t host_read_register(');b=s.index('static int host_read_index_admitted(',a)
a=s.index('static uint64_t host_read_register(');b=s.index('static int host_read_index_admitted(',a)
c=s.index('static void host_container_read_handoff(');e=s.index('static void *host_pump(',c)
code=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HOST_NGROUPS 2
#define EFS_RAFT_LEADER 2
struct efs_raft {uint64_t round,term,index,applied;int role,pending,done,fail;};
struct host_group {uint64_t read_admit_count,read_begin_admits,read_begin_round,read_begin_term;struct efs_raft *r;uint8_t group;};
struct server {int storage_engine;};
struct efs_raft_host {struct server *s;struct host_group g[2];};
static struct host_group *group_slot(struct efs_raft_host *h,uint8_t g){return g==0 || g==2?&h->g[g>>1]:NULL;}
static uint64_t efs_raft_read_round(struct efs_raft *r){return r->round;}
static uint64_t efs_raft_term(struct efs_raft *r){return r->term;}
static uint64_t efs_raft_read_index(struct efs_raft *r){return r->index;}
static int efs_raft_read_pending(struct efs_raft *r){return r->pending;}
static int efs_raft_read_done(struct efs_raft *r){return r->done;}
static int efs_raft_read_covers(struct efs_raft *r,uint64_t index){return r->done && r->applied>=index;}
static int efs_raft_role(struct efs_raft *r){return r->role;}
static uint64_t host_read_register(struct efs_raft_host *,uint8_t);
static struct efs_raft_host *current;static int late_send;static uint64_t late;
static int efs_raft_read_begin(struct efs_raft *r){
 if(r->fail)return r->fail;
 r->round++;r->pending=1;r->done=0;
 if(late_send)late=host_read_register(current,0);
 return 0;
}
'''+s[a:b]+s[c:e]+r'''
int main(void){
 struct server server={1};struct efs_raft r={.round=7,.term=3,.index=100,.applied=100,.role=EFS_RAFT_LEADER,.done=1};
 struct efs_raft_host h={.s=&server,.g={{.read_admit_count=4,.read_begin_admits=4,.read_begin_round=7,.read_begin_term=3,.r=&r,.group=0},{.group=2}}};current=&h;
 /* No new demand means no unsolicited background proof. */
 host_container_read_handoff(&h);assert(r.round==7);
 uint64_t ticket=host_read_register(&h,0);assert(ticket==5);
 struct host_group before=h.g[0];struct efs_raft initial=r;
 for(int guard=0;guard<9;guard++) {
  h.g[0]=before;r=initial;
  switch(guard){
   case 0:server.storage_engine=0;break;
   case 1:r.role=1;break;
   case 2:r.pending=1;break;
   case 3:r.done=0;break;
   case 4:r.applied=99;break;
   case 5:r.term++;break;
   case 6:h.g[0].read_begin_round=0;break;
   case 7:r.round=UINT64_MAX;h.g[0].read_begin_round=UINT64_MAX;break;
   case 8:r.fail=-2;break;
  }
  uint64_t round=r.round;host_container_read_handoff(&h);assert(r.round==round && h.g[0].read_begin_admits==4);
  server.storage_engine=1;
 }
 h.g[0]=before;r=initial;late_send=1;
 host_container_read_handoff(&h);
 assert(r.round==8 && r.pending && !r.done && h.g[0].read_begin_admits==5 && late==6);
 assert(host_read_eligible_round(&h,0,&r,ticket)==8);
 assert(host_read_eligible_round(&h,0,&r,late)==9);
 host_container_read_handoff(&h);assert(r.round==8); /* Never reset pending votes. */
 late_send=0;r.pending=0;r.done=1;r.applied=99;
 host_container_read_handoff(&h);assert(r.round==8); /* Full proof index, not wanted floor. */
 r.applied=100;host_container_read_handoff(&h);assert(r.round==9 && h.g[0].read_begin_admits==6);
 puts("container handoff: demand only, pending/full-apply/term/files/unknown/failure/max refusal, pre-send admission and late exclusion PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-handoff-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 for flags in [[],['-fsanitize=address,undefined','-fno-omit-frame-pointer']]:
  subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-pthread',*flags,str(p),'-o',str(p.with_suffix(''))],check=True)
  subprocess.run([str(p.with_suffix(''))],check=True,timeout=15)
 negative=code.replace('!efs_raft_read_covers(r,efs_raft_read_index(r))','0')
 assert negative!=code;p.write_text(negative)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 assert subprocess.run([str(p.with_suffix(''))],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode!=0
print('container handoff: ASan/UBSan and incomplete-apply negative PASS')

pump=s[s.index('static void *host_pump('):s.index('static void *host_pump(')+14000]
assert pump.index('host_container_read_handoff(h);')<pump.index('pthread_mutex_unlock(&h->mu);')
assert pump.index('host_container_read_handoff(h);')<pump.index('efs_kv_lsm_sync_release(h->kv)')
