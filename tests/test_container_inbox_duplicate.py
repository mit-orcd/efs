#!/usr/bin/env python3
"""Exercise actual opt-in inbox enqueue, fences, memory and credit accounting."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
def function(name):
 a=s.index(name);return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/raft_flow.h"
#include "efs/wire.h"
#include "efs/batch_obs.h"
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#define HOST_INBOX_MAX 256
#define HOST_INBOX_BYTES (32ull<<20)
#define HOST_FRAME_MAX (64ull<<20)
int efs_batch_obs_on;
void efs_batch_obs_count(enum efs_batch_counter k){(void)k;}
static int enabled=1,alloc_fail;
struct efsd_server {int storage_engine;};
struct host_inbox_item {uint8_t *buf;uint32_t len;uint64_t queued_us;int read_noted;};
struct efs_raft_host {
 struct efsd_server *s;int flow_enabled,running,raft_id,inbox_n,inbox_pressure;
 pthread_mutex_t inbox_mu;struct host_inbox_item inbox[256];
 uint64_t inbox_bytes,inbox_bytes_hi,inbox_work_n,inbox_work_bytes,inbox_drop;
 uint64_t inbox_probe_sent[2][32],inbox_probe_read[2][32];
};
static struct efs_raft_host *g_host;
static int env_on(const char *name){(void)name;return enabled;}
static uint64_t rd64be(const uint8_t *p){uint64_t v=0;for(int i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static int host_read_control(struct efs_raft_host *h,const uint8_t *p,uint32_t n){(void)h;(void)p;(void)n;return 0;}
static void host_pump_kick(struct efs_raft_host *h){(void)h;}
uint64_t efs_batch_obs_now_us(void){return 100;}
static int outbox_nentries(const uint8_t *p,uint32_t n){(void)n;return p[79];}
static void *test_malloc(size_t n){return alloc_fail?NULL:malloc(n);}
#define malloc test_malloc
'''+function('static int outbox_pure_read_control(')+function('static int host_inbox_duplicate_read(')+function('int server_raft_host_inbox(')+r'''
#undef malloc
static void clear(struct efs_raft_host *h){for(int i=0;i<h->inbox_n;i++)free(h->inbox[i].buf);h->inbox_n=0;h->inbox_bytes=0;}
static void send_frame(uint8_t *p){struct efs_raft_flow_reply f;assert(server_raft_host_inbox(p,80,&f)==EFS_OK);}
int main(void){
 struct efsd_server server={1};struct efs_raft_host h={.s=&server,.running=1,.inbox_mu=PTHREAD_MUTEX_INITIALIZER};g_host=&h;
 uint8_t p[80]={EFS_RAFT_MSG_AE_REP,0,1,1};p[35]=9;
 send_frame(p);h.inbox[0].read_noted=1;send_frame(p);
 assert(h.inbox_n==1&&h.inbox_bytes==80&&!h.inbox[0].read_noted);
 /* Preserve different groups and retain the last-arrival order. */
 p[1]=2;send_frame(p);p[1]=0;send_frame(p);
 assert(h.inbox_n==2&&h.inbox[0].buf[1]==2&&h.inbox[1].buf[1]==0&&h.inbox_bytes==160);
 /* Different contexts never fold, including equal-key older arrivals. */
 p[35]=10;send_frame(p);p[35]=9;send_frame(p);assert(h.inbox_n==3);
 clear(&h);send_frame(p);p[2]=0;send_frame(p);p[2]=1;send_frame(p);
 assert(h.inbox_n==3); /* Ordinary traffic is an ordering fence. */
 clear(&h);
 for(int field=0;field<80;field++){
  send_frame(p);uint8_t q[80];memcpy(q,p,80);q[field]++;
  assert(host_inbox_duplicate_read(&h,q,80)==-1);clear(&h);
 }
 p[0]=EFS_RAFT_MSG_AE_REQ;p[3]=0;send_frame(p);send_frame(p);assert(h.inbox_n==1);clear(&h);
 enabled=0;send_frame(p);send_frame(p);assert(h.inbox_n==2);clear(&h);enabled=1;
 server.storage_engine=0;send_frame(p);send_frame(p);assert(h.inbox_n==2);clear(&h);server.storage_engine=1;
 h.flow_enabled=1;send_frame(p);send_frame(p);assert(h.inbox_n==2&&h.inbox_work_n==2&&h.inbox_work_bytes==160);
 clear(&h);h.flow_enabled=0;h.inbox_work_n=0;h.inbox_work_bytes=0;
 send_frame(p);alloc_fail=1;struct efs_raft_flow_reply f;
 assert(server_raft_host_inbox(p,80,&f)==EFS_ERR_NOMEM&&h.inbox_n==1&&h.inbox_bytes==80);
 alloc_fail=0;clear(&h);
 /* Exact duplicate admission at capacity remains bounded, with no credits. */
 enabled=0;for(int i=0;i<256;i++)send_frame(p);enabled=1;
 send_frame(p);assert(h.inbox_n==256&&h.inbox_bytes==256*80&&h.inbox_work_n==0);
 p[35]++;assert(server_raft_host_inbox(p,80,&f)==EFS_ERR_BUSY&&h.inbox_n==256);
 clear(&h);pthread_mutex_destroy(&h.inbox_mu);puts("container exact-duplicate inbox safety PASS");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-container-inbox-') as d:
 p=Path(d)/'test.c';p.write_text(code);b=p.with_suffix('')
 flags=shlex.split(os.environ.get('EFS_TEST_CFLAGS',''))
 args=[os.environ.get('CC','cc'),'-std=c99','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),*flags]
 subprocess.run([*args,str(p),'-o',str(b)],check=True);subprocess.run([str(b)],check=True)
 p.write_text(code.replace('if (!memcmp(item->buf, payload, plen))','if (0)'))
 subprocess.run([*args,str(p),'-o',str(b)],check=True)
 assert subprocess.run([str(b)],capture_output=True).returncode!=0,'disabled-fold negative unexpectedly passed'
 print('disabled exact-fold negative refused as expected')
