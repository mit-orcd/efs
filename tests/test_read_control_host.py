#!/usr/bin/env python3
"""Quorum notes may bypass tail I/O; queued replication still receives ACKs."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/server/raft_host.c').read_text()
start=source.index('static int host_read_control(')
helper=source[start:source.index('\n}',start)+2]
code=r'''
#include "efs/raft.h"
#include "efs/wire.h"
#include "efs/batch_obs.h"
int efs_batch_obs_on;
void efs_batch_obs_count(enum efs_batch_counter k){(void)k;}
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
struct efs_raft_host {pthread_mutex_t mu,inbox_mu;int inbox_n;};
static int result,calls,published,woken,decode_fail;
int efs_wire_raft_decode(const uint8_t *p,uint32_t n,struct efs_raft_msg *m,uint8_t *b,uint32_t z) {
 (void)n;(void)b;(void)z;m->group=0;m->type=p[0];return decode_fail?EFS_ERR_PROTO:EFS_OK;
}
static struct efs_raft *group_raft(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;return NULL;}
int efs_raft_read_control(struct efs_raft *r,const struct efs_raft_msg *m){(void)r;(void)m;calls++;return result;}
static void host_publish_group(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;published++;}
static void host_waiters_wake(struct efs_raft_host *h){(void)h;woken++;}
'''+helper+r'''
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
 result=1;assert(!host_read_control(&h,p,sizeof(p)) && calls==8 && published==4 && woken==4 && h.inbox_n==1);
 result=0;assert(!host_read_control(&h,p,sizeof(p)) && calls==9 && published==4);
 p[0]=EFS_RAFT_MSG_AE_REQ;result=1;
 assert(!host_read_control(&h,p,sizeof(p)) && calls==10 && h.inbox_n==1);
 result=2;assert(!host_read_control(&h,p,sizeof(p)) && calls==11 && h.inbox_n==1);
 result=0;assert(!host_read_control(&h,p,sizeof(p)) && calls==12 && h.inbox_n==1);
 /* Requests never consume pending frames, even when otherwise accounted;
  * refusal never publishes progress or changes queue ownership. */
 h.inbox_n=0;
 assert(!pthread_mutex_trylock(&h.mu));pthread_mutex_unlock(&h.mu);
 assert(!pthread_mutex_trylock(&h.inbox_mu));pthread_mutex_unlock(&h.inbox_mu);
 pthread_mutex_destroy(&h.mu);pthread_mutex_destroy(&h.inbox_mu);
 puts("read control host: busy mutex, pending order, frame refusal, early request votes and reply bookkeeping PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-control-host-') as d:
    c=Path(d)/'test.c';c.write_text(code);binary=c.with_suffix('')
    subprocess.run(['cc','-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),str(c),'-pthread','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
