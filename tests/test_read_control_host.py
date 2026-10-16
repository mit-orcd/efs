#!/usr/bin/env python3
"""Quorum notes may bypass tail I/O; queued replication still receives ACKs."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/server/raft_host.c').read_text()
start=source.index('static int host_read_control(')
helper=source[start:source.index('\n}',start)+2]
pure=source.index('static int outbox_pure_read_control(');pure_end=source.index('\n}',pure)+2
code=r'''
#include "efs/raft.h"
#include "efs/wire.h"
#include "efs/batch_obs.h"
int efs_batch_obs_on;
static unsigned counts[BO_COUNTER_N];
void efs_batch_obs_count(enum efs_batch_counter k){counts[k]++;}
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct efsd_server {int unused;};
struct efs_raft_host {pthread_mutex_t mu,inbox_mu;int inbox_n;struct efsd_server *s;};
static int result,calls,published,woken,decode_fail;
static uint64_t read_context;
int efs_wire_raft_decode(const uint8_t *p,uint32_t n,struct efs_raft_msg *m,uint8_t *b,uint32_t z) {
 (void)n;(void)b;(void)z;memset(m,0,sizeof(*m));m->group=0;m->type=p[0];m->last_log_index=read_context;m->vote_granted=p[2];m->success=p[3];m->prev_index=p[36];m->prev_term=p[44];m->leader_commit=p[52];m->match_index=p[60];m->last_log_term=p[68];m->nentries=p[76];return decode_fail?EFS_ERR_PROTO:EFS_OK;
}
static struct efs_raft *group_raft(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;return NULL;}
int efs_raft_read_control(struct efs_raft *r,const struct efs_raft_msg *m){(void)r;(void)m;calls++;return result;}
static void host_publish_group(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;published++;}
static void host_waiters_wake(struct efs_raft_host *h){(void)h;woken++;}
/* A handled ACK may free the next requested read round; it is started
 * before the publish, under the h->mu this path already holds. */
static int starts;
static unsigned host_read_start_requested(struct efs_raft_host *h){assert(pthread_mutex_trylock(&h->mu)!=0);starts++;return 0;}
static uint64_t rd64be(const uint8_t*p){uint64_t v=0;for(int i=0;i<8;i++)v=(v<<8)|p[i];return v;}
'''+source[pure:pure_end]+helper+r'''
int main(void) {
 struct efs_raft_host h={0};uint8_t p[EFS_WIRE_RAFT_HDR_LEN]={EFS_RAFT_MSG_AE_REQ};
 pthread_mutex_init(&h.mu,NULL);pthread_mutex_init(&h.inbox_mu,NULL);result=1;
 assert(!host_read_control(&h,p,sizeof(p)-1) && !calls);
 p[0]=EFS_RAFT_MSG_VOTE_REQ;assert(!host_read_control(&h,p,sizeof(p)) && !calls);
 p[0]=EFS_RAFT_MSG_AE_REQ;decode_fail=1;
 assert(!host_read_control(&h,p,sizeof(p)) && !calls);decode_fail=0;
 pthread_mutex_lock(&h.mu);assert(!host_read_control(&h,p,sizeof(p)) && !calls);
 pthread_mutex_unlock(&h.mu);
 h.inbox_n=1;assert(!host_read_control(&h,p,sizeof(p)) && calls==1 && h.inbox_n==1);h.inbox_n=0;
 assert(host_read_control(&h,p,sizeof(p))==1 && calls==2 && !published && !woken);
 p[0]=EFS_RAFT_MSG_AE_REP;result=2;
 assert(!host_read_control(&h,p,sizeof(p)) && calls==3 && published==1 && woken==1);
 /* A noted reply still returns fallback so normal recv owns progress. */
 result=1;assert(host_read_control(&h,p,sizeof(p)) && calls==4 && published==2 && woken==2);
 /* Fully accounted probe replies leave no pending bookkeeping. */
 result=0;assert(!host_read_control(&h,p,sizeof(p)) && calls==5 && published==2);
 p[0]=EFS_RAFT_MSG_AE_REQ;result=2;
 assert(!host_read_control(&h,p,sizeof(p)) && calls==6 && published==2 && woken==2);
 /* Early follower ACK leaves commit/apply on the normal ordered inbox. */
 h.inbox_n=1;p[0]=EFS_RAFT_MSG_AE_REP;result=2;
 assert(!host_read_control(&h,p,sizeof(p)) && calls==7 && published==3 && woken==3 && h.inbox_n==1);
 result=1;assert(host_read_control(&h,p,sizeof(p)) && calls==8 && published==4 && woken==4 && h.inbox_n==1);
 result=0;assert(!host_read_control(&h,p,sizeof(p)) && calls==9 && published==4);
 p[0]=EFS_RAFT_MSG_AE_REQ;result=1;
 assert(!host_read_control(&h,p,sizeof(p)) && calls==10 && h.inbox_n==1);
 result=2;assert(!host_read_control(&h,p,sizeof(p)) && calls==11 && h.inbox_n==1);
 result=0;assert(!host_read_control(&h,p,sizeof(p)) && calls==12 && h.inbox_n==1);
 /* Requests never consume pending frames, even when otherwise accounted;
  * accounted replies consume no queue slot; refusal never publishes progress. */
 struct efsd_server server={.unused=0};h.s=&server;read_context=42;
 /* Exact read-only prefix-zero probes can retire after result1, without
  * touching queued bytes/credits. All other headers still owe receive. */
 p[2]=1;result=1;
 assert(host_read_control(&h,p,sizeof(p)) && h.inbox_n==1);
 result=2;assert(!host_read_control(&h,p,sizeof(p)) && h.inbox_n==1);
 result=0;assert(!host_read_control(&h,p,sizeof(p)) && h.inbox_n==1);
 result=1;
 const int fields[]={3,36,44,52,60,68,76};
 for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++){
  p[fields[i]]=1;assert(!host_read_control(&h,p,sizeof(p)) && h.inbox_n==1);p[fields[i]]=0;
 }
 p[2]=2;assert(!host_read_control(&h,p,sizeof(p)));p[2]=0;
 assert(!host_read_control(&h,p,sizeof(p)));p[2]=1;
 assert(host_read_control(&h,p,sizeof(p)) && h.inbox_n==1);
 assert(published==4 && woken==4 && starts==4);
 h.inbox_n=0;
 assert(!pthread_mutex_trylock(&h.mu));pthread_mutex_unlock(&h.mu);
 assert(!pthread_mutex_trylock(&h.inbox_mu));pthread_mutex_unlock(&h.inbox_mu);
 memset(p,0,sizeof(p));p[0]=EFS_RAFT_MSG_AE_REQ;p[2]=1;p[35]=7;read_context=7;result=1;
 efs_batch_obs_on=1;h.s=&server;
 assert(host_read_control(&h,p,sizeof(p)) && counts[BO_READ_REQ_INLINE]==1);
 p[0]=EFS_RAFT_MSG_AE_REP;p[3]=1;assert(host_read_control(&h,p,sizeof(p)) && counts[BO_READ_REPLY_INLINE]==1);
 efs_batch_obs_on=0;assert(host_read_control(&h,p,sizeof(p)) && counts[BO_READ_REPLY_INLINE]==1);
 efs_batch_obs_on=1;server.unused=0;assert(host_read_control(&h,p,sizeof(p)) && counts[BO_READ_REPLY_INLINE]==2);
 result=0;assert(!host_read_control(&h,p,sizeof(p)) && counts[BO_READ_REPLY_INLINE]==2);
 pthread_mutex_destroy(&h.mu);pthread_mutex_destroy(&h.inbox_mu);
 puts("read control host: busy mutex, pending order, frame refusal, early request votes and reply bookkeeping PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-control-host-') as d:
    c=Path(d)/'test.c';c.write_text(code);binary=c.with_suffix('')
    subprocess.run(['cc','-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',
                    *shlex.split(os.environ.get('EFS_TEST_CFLAGS','')), '-I'+str(root/'include'),str(c),'-pthread','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    c.write_text(code.replace('if(pending && handled==1 && msg.type==EFS_RAFT_MSG_AE_REQ && !pure_probe)', '(void)pure_probe; if(pending && handled==1)'))
    subprocess.run(['cc','-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),str(c),'-pthread','-o',str(binary)],check=True)
    negative=subprocess.run([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    assert negative.returncode, 'forced FIFO retention negative unexpectedly passed'
    print('forced pure-probe FIFO retention negative refused as expected')
