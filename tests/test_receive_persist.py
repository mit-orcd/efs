#!/usr/bin/env python3
"""Real Raft core plus actual host barrier handoff: no early authority, other-group progress."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('static int host_receive_persist(');hook=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/raft.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define HOST_NGROUPS 2
struct efs_raft_host {pthread_mutex_t mu;struct efs_raft *r[2];struct {int hosted;} g[2];unsigned acks[2],votes[2],applies[2],published,woken,read_votes;int fail_read_vote;uint64_t published_commit[2];};
static void host_publish_view(struct efs_raft_host*h,int i){h->published++;h->published_commit[i]=efs_raft_commit(h->r[i]);}
static void host_waiters_wake(struct efs_raft_host*h){h->woken++;}
'''+hook+r'''
struct gate {struct efs_raft_store *mem,ops;int enabled,started,release,fail;uint64_t durable;};
static struct gate gates[2];
static pthread_mutex_t wait_mu=PTHREAD_MUTEX_INITIALIZER;
static struct gate *gate(void *ctx){for(int i=0;i<2;i++)if(gates[i].mem==ctx)return &gates[i];abort();}
static int begin(void *ctx){(void)ctx;return EFS_OK;}
static int end(void *ctx){struct gate*g=gate(ctx);
 if(g==&gates[0]&&g->enabled){
  pthread_mutex_lock(&wait_mu);g->started=1;pthread_mutex_unlock(&wait_mu);
  for(;;){pthread_mutex_lock(&wait_mu);int go=g->release;pthread_mutex_unlock(&wait_mu);if(go)break;usleep(1000);}
 }
 if(g->fail)return EFS_ERR_IO;
 uint64_t term;assert(g->mem->last(g->mem,&g->durable,&term)==EFS_OK);return EFS_OK;
}
static int send(void *ctx,const struct efs_raft_msg*m){struct efs_raft_host*h=ctx;int i=m->group==2;
 if(m->type==EFS_RAFT_MSG_AE_REP && m->success && m->match_index){assert(m->match_index<=gates[i].durable);h->acks[i]++;}
 if(m->type==EFS_RAFT_MSG_AE_REP && m->vote_granted && m->success && !m->match_index && m->last_log_index){if(h->fail_read_vote)return EFS_ERR_IO;h->read_votes++;}
 if(m->type==EFS_RAFT_MSG_VOTE_REP&&m->vote_granted)h->votes[i]++;
 return EFS_OK;
}
struct app {struct efs_raft_host*h;int i;};
static int apply(void *ctx,uint64_t index,uint64_t term,const uint8_t*cmd,uint32_t len){struct app*a=ctx;(void)term;assert(index<=gates[a->i].durable && len==1 && *cmd=='x');a->h->applies[a->i]++;return EFS_OK;}
struct receiver {struct efs_raft_host*h;struct efs_raft_msg m[2];int mode,rc;};
static void *receive(void *ctx){struct receiver*x=ctx;struct efs_raft_msg m=x->m[0];
 if(x->mode==2){m.nentries=2;m.entries[1]=m.entries[0];m.leader_commit=2;}
 if(x->mode==4){m.type=EFS_RAFT_MSG_VOTE_REQ;m.from=2;m.term=3;m.nentries=0;m.leader_commit=0;}
 pthread_mutex_lock(&x->h->mu);x->rc=x->mode?efs_raft_recv(x->h->r[0],&m):efs_raft_recv_batch(x->h->r[0],x->m,2);pthread_mutex_unlock(&x->h->mu);return NULL;}

static void blocked(struct efs_raft*r,struct efs_raft_msg*m,uint64_t expected_term){uint64_t index=99,round=99;
 assert(efs_raft_tick(r)==EFS_ERR_BUSY);assert(efs_raft_recv(r,m)==EFS_ERR_BUSY);
 assert(efs_raft_recv_batch(r,m,1)==EFS_ERR_BUSY);
 assert(efs_raft_propose(r,(uint8_t*)"x",1,&index)==EFS_ERR_BUSY);
 assert(efs_raft_propose_local(r,(uint8_t*)"x",1,&index)==EFS_ERR_BUSY);
 assert(efs_raft_submit(r,2)==EFS_ERR_BUSY&&efs_raft_flush(r)==EFS_ERR_BUSY);
 assert(efs_raft_note_durable(r,2)==EFS_ERR_BUSY&&efs_raft_durable(r,2)==EFS_ERR_BUSY);
 assert(efs_raft_change(r,3)==EFS_ERR_BUSY&&efs_raft_restore_applied(r,2)==EFS_ERR_BUSY);
 assert(efs_raft_snapshot(r)==EFS_ERR_BUSY&&efs_raft_step_down(r)==EFS_ERR_BUSY);
 assert(efs_raft_transfer(r,0)==EFS_ERR_BUSY&&efs_raft_read_begin(r)==EFS_ERR_BUSY);
 efs_raft_arm_durable(r); /* void entry cannot change durable/send coverage */
 struct efs_raft_msg ordinary=*m;ordinary.leader_commit=1;
 assert(!efs_raft_read_control(r,&ordinary));
 assert(!efs_raft_read_ready(r)&&!efs_raft_read_current(r)&&!efs_raft_read_pending(r));
 assert(!efs_raft_read_quorum(r)&&!efs_raft_read_done(r)&&!efs_raft_read_covers(r,0));
 assert(!efs_raft_read_proven(r,&round,&index)&&!round&&!index);
 assert(!efs_raft_read_index(r)&&!efs_raft_read_context(r)&&!efs_raft_read_round(r));
 assert(efs_raft_term(r)==expected_term&&efs_raft_role(r)==EFS_RAFT_FOLLOWER&&efs_raft_voters(r)==7);
 assert(!efs_raft_commit(r)&&!efs_raft_applied(r));
}
int main(void){for(int mode=0;mode<5;mode++)for(int failure=0;failure<2;failure++){
 struct efs_raft_host h={0};struct app apps[2];struct receiver x={.h=&h,.mode=mode};pthread_mutex_init(&h.mu,NULL);memset(gates,0,sizeof(gates));
 for(int i=0;i<2;i++){
  gates[i].mem=efs_raft_mem_create();assert(gates[i].mem);gates[i].ops=*gates[i].mem;gates[i].ops.batch_begin=begin;gates[i].ops.batch_end=end;
  struct efs_raft_cfg cfg={0};cfg.id=1;cfg.n=3;cfg.group=i?2:0;cfg.store=&gates[i].ops;cfg.store_ctx=gates[i].mem;cfg.send=send;cfg.net=&h;cfg.recv_persist=host_receive_persist;
  apps[i]=(struct app){&h,i};cfg.apply=apply;cfg.app=&apps[i];h.r[i]=efs_raft_new(&cfg);assert(h.r[i]);h.g[i].hosted=1;
  struct efs_raft_msg m={0};m.type=EFS_RAFT_MSG_AE_REQ;m.from=0;m.to=1;m.term=2;m.boot_id=9;m.group=cfg.group;
  pthread_mutex_lock(&h.mu);assert(efs_raft_recv(h.r[i],&m)==EFS_OK);pthread_mutex_unlock(&h.mu);
  if(!i)for(int j=0;j<2;j++){x.m[j]=m;x.m[j].prev_index=j;x.m[j].prev_term=j?2:0;x.m[j].nentries=1;x.m[j].entries[0]=(struct efs_raft_entry){2,1,(uint8_t*)"x"};x.m[j].leader_commit=j+1;x.m[j].last_log_index=10+j;}
 }
 if(mode==3)assert(gates[0].mem->append(gates[0].mem,1,1,(uint8_t*)"x",1)==EFS_OK);
 h.published=h.woken=0;gates[0].enabled=1;gates[0].fail=failure;pthread_t tid;assert(!pthread_create(&tid,NULL,receive,&x));
 int began=0;for(int n=0;n<1000;n++){pthread_mutex_lock(&wait_mu);began=gates[0].started;pthread_mutex_unlock(&wait_mu);if(began)break;usleep(1000);}assert(began);
 int entered=0;for(int n=0;n<1000;n++){if(!pthread_mutex_trylock(&h.mu)){entered=1;break;}usleep(1000);}
 if(entered){
  assert(h.published&&h.woken&&!h.published_commit[0]);
  assert(!h.acks[0]&&!h.votes[0]&&!h.applies[0]);struct efs_raft_msg probe=x.m[0];probe.nentries=0;probe.prev_index=probe.prev_term=probe.leader_commit=0;probe.last_log_index=42;probe.vote_granted=1;probe.term=mode==4?3:2;
  blocked(h.r[0],&probe,probe.term);
  h.fail_read_vote=1;assert(!efs_raft_read_control(h.r[0],&probe)&&!h.read_votes);h.fail_read_vote=0;
  assert(efs_raft_read_control(h.r[0],&probe)==(mode<=2));
  assert(h.read_votes==(unsigned)(mode<=2));
  const unsigned fields[]={0,1,2,3,4,5,6,7};
  for(unsigned j=0;j<sizeof(fields)/sizeof(fields[0]);j++){
   struct efs_raft_msg bad=probe;
   switch(fields[j]){case 0:bad.term++;break;case 1:bad.boot_id++;break;
    case 2:bad.from=2;break;case 3:bad.prev_index=1;break;
    case 4:bad.leader_commit=1;break;case 5:bad.vote_granted=0;break;
    case 6:bad.last_log_index=0;break;default:bad.group=2;break;}
   assert(!efs_raft_read_control(h.r[0],&bad));
  }
  assert(h.read_votes==(unsigned)(mode<=2) && !h.acks[0] && !h.applies[0]);
  assert(!efs_raft_read_ready(h.r[0]) && !efs_raft_read_quorum(h.r[0]));
  struct efs_raft_msg other[2]={x.m[0],x.m[1]};other[0].group=other[1].group=2;
  assert(efs_raft_recv_batch(h.r[1],other,2)==EFS_OK&&h.acks[1]==2&&h.applies[1]==2);
  assert(!h.acks[0]&&!h.applies[0]);pthread_mutex_unlock(&h.mu);
 }
 pthread_mutex_lock(&wait_mu);gates[0].release=1;pthread_mutex_unlock(&wait_mu);assert(!pthread_join(tid,NULL));
 assert(entered && "host lock must be available while receive persistence is stalled");
 pthread_mutex_lock(&h.mu);
 if(failure){assert(x.rc==EFS_ERR_IO&&!h.acks[0]&&!h.votes[0]&&!h.applies[0]&&!efs_raft_commit(h.r[0]));assert(efs_raft_recv(h.r[0],&x.m[0])==EFS_ERR_IO);}
 else {unsigned applied=mode==4?0:mode==0||mode==2?2:1;unsigned acks=mode==4?0:mode==0?2:1;
  assert(x.rc==EFS_OK&&h.acks[0]==acks&&h.votes[0]==(unsigned)(mode==4)&&h.applies[0]==applied&&efs_raft_commit(h.r[0])==applied);
  struct efs_raft_msg higher=x.m[0];higher.nentries=0;higher.term=mode==4?4:3;higher.prev_index=applied;higher.prev_term=applied?2:0;higher.leader_commit=applied;
  assert(efs_raft_recv(h.r[0],&higher)==EFS_OK&&efs_raft_term(h.r[0])==higher.term);}

 pthread_mutex_unlock(&h.mu);
 for(int i=0;i<2;i++){efs_raft_free(h.r[i]);efs_raft_mem_free(gates[i].mem);}pthread_mutex_destroy(&h.mu);
 }
 puts("receive persistence: cohort/single/multi/conflict/hard-state handoff, frozen-group authority, other-group progress and failed-sync fencing PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-recv-persist-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    core=Path(os.environ.get('EFS_RAFT_CORE_SOURCE',root/'src/raft/raft.c'))
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(core),str(root/'src/raft/raft_mem.c'),str(root/'src/common/batch_obs.c'),'-pthread','-o',str(p.with_suffix(''))]
    subprocess.run(cmd,check=True);subprocess.run([str(p.with_suffix(''))],check=True,timeout=15)
