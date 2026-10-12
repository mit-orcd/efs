#!/usr/bin/env python3
"""Actual follower proof coalescer: bounded overlap, fresh admission, late replies."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('struct host_ri_coal {');b=s.index('\n/* ReadIndex for one group',a)
code=r'''
#include "efs/common.h"
#include "efs/protocol.h"
#include <pthread.h>
#include <assert.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#define HOST_NGROUPS 2
struct efs_raft_host {int unused;};
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int begun,released[8],active,high,waiting;
static int host_remote_cmd(struct efs_raft_host *h,uint8_t g,const void *p,uint32_t n,
 struct efs_msg_raft_mkfs_reply *r,int leader) {
 (void)h;(void)g;(void)p;(void)n;(void)leader;
 pthread_mutex_lock(&gate);int ticket=++begun;assert(ticket<8);active++;
 if(active>high)high=active;
 pthread_cond_broadcast(&changed);
 while(!released[ticket])pthread_cond_wait(&changed,&gate);
 active--;pthread_mutex_unlock(&gate);r->index=100+ticket;r->leader_hint=1;
 return ticket==4?EFS_ERR_IO:EFS_OK;
}
static int watch_wait(pthread_cond_t *cv,pthread_mutex_t *mu,const struct timespec *end) {
 pthread_mutex_lock(&gate);waiting++;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
 return pthread_cond_timedwait(cv,mu,end);
}
#define pthread_cond_timedwait watch_wait
'''+s[a:b]+r'''
#undef pthread_cond_timedwait
struct result {int rc,hint;uint64_t index;};
static struct efs_raft_host host;
static void *reader(void *p){struct result *r=p;struct timespec end;clock_gettime(CLOCK_REALTIME,&end);end.tv_sec+=15;
 r->rc=host_remote_read_index(&host,0,1,&end,&r->index,&r->hint);return NULL;}
static void until(int calls,int waiters) {
 pthread_mutex_lock(&gate);while(begun<calls || waiting<waiters)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);
}
static void release(int ticket){pthread_mutex_lock(&gate);released[ticket]=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);}
int main(void){
 pthread_t a,b,c,d,e;struct result ra={0},rb={0},rc={0},rd={0},re={0};
 pthread_create(&a,NULL,reader,&ra);until(1,0);
 pthread_create(&b,NULL,reader,&rb);until(2,0); /* second reaches leader while first held */
 pthread_create(&c,NULL,reader,&rc);until(2,1);
 pthread_mutex_lock(&gate);assert(begun==2 && active==2 && high==2);pthread_mutex_unlock(&gate);
 release(2);pthread_join(b,NULL);assert(rb.rc==0 && rb.index==102);
 until(3,1); /* next fresh admission may overlap the still held first */
 pthread_create(&d,NULL,reader,&rd);until(3,2);
 release(3);until(4,2);pthread_join(c,NULL);assert(rc.rc==0 && rc.index==103);
 release(4);pthread_join(d,NULL);assert(rd.rc==EFS_ERR_IO && !rd.index && rd.hint==-1);
 release(1);pthread_join(a,NULL); /* late older success cannot replace newer failure */
 assert(ra.rc==EFS_ERR_IO && !ra.index && ra.hint==-1 && high==2);
 pthread_create(&e,NULL,reader,&re);until(5,2);release(5);pthread_join(e,NULL);
 assert(re.rc==0 && re.index==105 && re.hint==1); /* never reuses finished proof */
 g_ri_coal[0].started=UINT64_MAX;g_ri_coal[0].finished=UINT64_MAX;
 struct timespec end;clock_gettime(CLOCK_REALTIME,&end);end.tv_sec++;
 assert(host_remote_read_index(&host,0,1,&end,&re.index,&re.hint)==EFS_ERR_BUSY);
 puts("remote read window: two owned RPCs, later admission, FIFO freshness floor, out-of-order/error results and overflow PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-remote-read-window-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=20)
