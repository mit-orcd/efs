#!/usr/bin/env python3
"""Early inbox votes retain ordered receive and credit ownership."""
from pathlib import Path
import os, shlex, subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('static void host_pending_reads(');b=s.index('\n}',a)+2
pure=s.index('static int outbox_pure_read_control(');pure_end=s.index('\n}',pure)+2
code=r'''
#include "efs/raft.h"
#include "efs/wire.h"
#include "efs/batch_obs.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
static unsigned discarded;
static void discard(void*p){discarded++;free(p);}
#define free discard
#define HOST_INBOX_MAX 16
#define HOST_NGROUPS 2
int efs_batch_obs_on;
void efs_batch_obs_count(enum efs_batch_counter c){(void)c;}
struct server{int unused;};
struct host_inbox_item{uint8_t*buf;uint32_t len;uint64_t queued_us;int read_noted;};
struct efs_raft_host{struct server*s;pthread_mutex_t inbox_mu;int inbox_n,flow_enabled;uint64_t inbox_bytes,inbox_work_n,inbox_work_bytes;struct host_inbox_item inbox[HOST_INBOX_MAX];struct{uint8_t group;}g[2];};
static struct efs_raft_host *current;
static int calls,decoded,published,woken,expected_result,forced_result=-1,append_race;
static struct host_inbox_item arrival;
static unsigned clocks, samples[BH_HIST_N];
static uint64_t last_value;
uint64_t efs_batch_obs_now_us(void){clocks++;return 1000;}
void efs_batch_obs_hist(enum efs_batch_hist h,uint64_t v){samples[h]++;last_value=v;}
static uint64_t rd64be(const uint8_t*p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
int efs_wire_raft_decode(const uint8_t*p,uint32_t n,struct efs_raft_msg*m,uint8_t*c,uint32_t z){(void)n;(void)c;(void)z;decoded++;if(p[2]==255)return EFS_ERR_PROTO;memset(m,0,sizeof(*m));m->type=p[0];m->group=p[1];m->last_log_index=rd64be(p+28);expected_result=forced_result>=0?forced_result:p[3];return EFS_OK;}
static struct efs_raft*group_raft(struct efs_raft_host*h,uint8_t g){(void)h;return (struct efs_raft*)(uintptr_t)(g+1);}
int efs_raft_read_control(struct efs_raft*r,const struct efs_raft_msg*m){(void)r;(void)m;assert(!pthread_mutex_trylock(&current->inbox_mu));pthread_mutex_unlock(&current->inbox_mu);if(append_race){pthread_mutex_lock(&current->inbox_mu);current->inbox[current->inbox_n++]=arrival;current->inbox_bytes+=arrival.len;pthread_mutex_unlock(&current->inbox_mu);append_race=0;}calls++;return expected_result;}
static void host_publish_group(struct efs_raft_host*h,uint8_t g){assert(h==current&&g==2);published++;}
static void host_waiters_wake(struct efs_raft_host*h){assert(h==current);woken++;}
'''+s[pure:pure_end]+s[a:b]+r'''
int main(int argc,char **argv){
 struct server s={argc>1?atoi(argv[1]):0};struct efs_raft_host h={.s=&s,.inbox_n=7,.inbox_bytes=572,.inbox_work_n=7,.inbox_work_bytes=572};current=&h;h.g[1].group=2;
 assert(!pthread_mutex_init(&h.inbox_mu,NULL));uint8_t (*p)[92]=calloc(HOST_INBOX_MAX,92),saved[HOST_INBOX_MAX][92];assert(p);
 for(int i=0;i<7;i++){p[i][0]=EFS_RAFT_MSG_AE_REQ;p[i][3]=1;p[i][35]=7;h.inbox[i]=(struct host_inbox_item){p[i],80,(uint64_t)i,0};}
 p[1][0]=EFS_RAFT_MSG_AE_REP;p[1][1]=2;p[1][3]=2;
 p[2][0]=EFS_RAFT_MSG_VOTE_REQ; /* never accelerate term/config controls */
 h.inbox[3].len=92; /* data remains ordered even with a read context */
 p[4][35]=0; /* zero-context heartbeat */
 p[5][2]=255; /* decoder failure */
 p[6][3]=0; /* core refuses: e.g. prefix/term/boot not yet eligible */
 memcpy(saved,p,sizeof(saved));
 h.s=NULL;host_pending_reads(&h);assert(!calls&&!decoded&&!published&&!woken);h.s=&s;
 host_pending_reads(&h);assert(calls==3&&decoded==4&&published==1&&woken==1);
 assert(h.inbox[0].read_noted&&h.inbox[1].read_noted&&!h.inbox[6].read_noted);
 assert(h.inbox_n==7&&h.inbox_bytes==572&&h.inbox_work_n==7&&h.inbox_work_bytes==572);
 assert(!memcmp(p,saved,sizeof(saved)));
 for(int i=0;i<7;i++)assert(h.inbox[i].buf==p[i]&&h.inbox[i].queued_us==(uint64_t)i);
 host_pending_reads(&h);assert(calls==4&&published==1&&woken==1); /* accepted votes do not repeat while retained */
 p[6][3]=1;host_pending_reads(&h);assert(calls==5&&h.inbox[6].read_noted&&published==1&&woken==1);
 /* A fresh enqueue at a reused slot has no inherited note. */
 h.inbox[0].read_noted=0;p[0][35]=8;host_pending_reads(&h);assert(calls==6&&h.inbox[0].read_noted);
 h.inbox_n=HOST_INBOX_MAX;calls=decoded=published=woken=0;
 for(int i=0;i<HOST_INBOX_MAX;i++){
  memset(p[i],0,sizeof(p[i]));p[i][0]=EFS_RAFT_MSG_AE_REQ;p[i][3]=1;p[i][35]=9;
  h.inbox[i]=(struct host_inbox_item){p[i],80,(uint64_t)i,0};
 }
 host_pending_reads(&h);assert(calls==HOST_INBOX_MAX&&decoded==HOST_INBOX_MAX&&!published&&!woken);
 for(int i=0;i<HOST_INBOX_MAX;i++)assert(h.inbox[i].read_noted&&h.inbox[i].buf==p[i]);
 assert(h.inbox_n==HOST_INBOX_MAX&&h.inbox_work_n==7&&h.inbox_work_bytes==572);
 /* Accepted queued controls are timed once; refusal and diagnostics off
  * must not consume a sample or clock, and bytes/credits stay untouched. */
 h.inbox_n=1;h.inbox_bytes=80;uint8_t *canonical=calloc(1,92);assert(canonical);
 canonical[0]=EFS_RAFT_MSG_AE_REQ;canonical[2]=1;canonical[35]=7;
 h.inbox[0]=(struct host_inbox_item){canonical,80,100,0};efs_batch_obs_on=1;forced_result=0;
 host_pending_reads(&h);assert(!clocks&&!samples[BH_READ_REQ_INBOX_US]&&!h.inbox[0].read_noted&&!discarded);
 forced_result=1;host_pending_reads(&h);assert(clocks==1&&samples[BH_READ_REQ_INBOX_US]==1&&last_value==900);
 assert(!h.inbox_n&&!h.inbox_bytes&&discarded==1);
 host_pending_reads(&h);assert(clocks==1&&samples[BH_READ_REQ_INBOX_US]==1&&discarded==1);
 canonical=calloc(1,92);assert(canonical);canonical[0]=EFS_RAFT_MSG_AE_REP;canonical[1]=2;
 canonical[2]=1;canonical[3]=1;canonical[35]=7;
 h.inbox[0]=(struct host_inbox_item){canonical,80,100,0};h.inbox_n=1;h.inbox_bytes=80;
 host_pending_reads(&h);assert(clocks==2&&samples[BH_READ_REPLY_INBOX_US]==1&&h.inbox_n==1&&discarded==1);
 efs_batch_obs_on=0;h.inbox[0].read_noted=0;host_pending_reads(&h);assert(clocks==2);
 efs_batch_obs_on=1;canonical[67]=1;h.inbox[0].read_noted=0;host_pending_reads(&h);assert(clocks==2);
 canonical[67]=0;h.inbox[0].queued_us=0;h.inbox[0].read_noted=0;host_pending_reads(&h);assert(clocks==2);
 /* Active flow preserves admitted frames/credits, including canonical ones. */
 h.flow_enabled=1;canonical[0]=EFS_RAFT_MSG_AE_REQ;canonical[3]=0;
 h.inbox[0].read_noted=0;host_pending_reads(&h);
 assert(h.inbox_n==1&&h.inbox_bytes==80&&discarded==1);
 assert(h.inbox_work_n==7&&h.inbox_work_bytes==572);
 /* Partially handled canonical requests still owe normal receive/apply. */
 h.flow_enabled=0;h.inbox[0].read_noted=0;forced_result=2;
 host_pending_reads(&h);assert(h.inbox_n==1&&h.inbox_bytes==80&&discarded==1);
 uint8_t *partial=canonical;
 /* Compact multiple completed requests exactly once, preserving an old
  * data frame and an arrival appended during core handling. */
 uint8_t *a=calloc(1,92),*b=calloc(1,92);assert(a&&b);
 a[0]=b[0]=EFS_RAFT_MSG_AE_REQ;a[2]=b[2]=1;a[35]=7;b[35]=8;
 h.inbox_n=3;h.inbox_bytes=252;h.inbox[0]=(struct host_inbox_item){a,80,100,0};
 h.inbox[1]=(struct host_inbox_item){p[1],92,0,0};h.inbox[2]=(struct host_inbox_item){b,80,100,0};
 arrival=(struct host_inbox_item){p[2],92,0,0};append_race=1;forced_result=1;efs_batch_obs_on=0;
 host_pending_reads(&h);
 assert(!append_race&&discarded==3&&h.inbox_n==2&&h.inbox_bytes==184);
 assert(h.inbox[0].buf==p[1]&&h.inbox[1].buf==p[2]&&!h.inbox[0].read_noted&&!h.inbox[1].read_noted);
 assert(h.inbox_work_n==7&&h.inbox_work_bytes==572);
 /* A canonical request arriving during the first bounded pass is owned
  * only by the next pass; ordinary data and credits keep their order. */
 uint8_t *late=calloc(1,80),*first=calloc(1,80);assert(late&&first);
 late[0]=first[0]=EFS_RAFT_MSG_AE_REQ;late[2]=first[2]=1;late[35]=9;first[35]=10;
 h.inbox_n=2;h.inbox_bytes=172;
 h.inbox[0]=(struct host_inbox_item){p[1],92,0,0};
 h.inbox[1]=(struct host_inbox_item){first,80,0,0};
 arrival=(struct host_inbox_item){late,80,0,0};append_race=1;
 int before=calls;host_pending_reads(&h);
 assert(calls==before+1&&discarded==4&&h.inbox_n==2&&h.inbox_bytes==172);
 assert(h.inbox[0].buf==p[1]&&h.inbox[1].buf==late&&!h.inbox[1].read_noted);
 host_pending_reads(&h);
 assert(calls==before+2&&discarded==5&&h.inbox_n==1&&h.inbox_bytes==92);
 assert(h.inbox[0].buf==p[1]&&!h.inbox[0].read_noted);
 host_pending_reads(&h);assert(calls==before+2&&discarded==5);
 assert(h.inbox_work_n==7&&h.inbox_work_bytes==572);
#undef free
 free(partial);free(p);pthread_mutex_destroy(&h.inbox_mu);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-raft-pending-read-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run([os.environ.get('CC','cc'),*shlex.split(os.environ.get('EFS_TEST_CFLAGS','')),'-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-lpthread','-o',str(binary)],check=True)
 for engine in ("0","1"):subprocess.run([str(binary),engine],check=True)
 p.write_text(code.replace('if(consume_n){','if(0 && consume_n){'))
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-lpthread','-o',str(binary)],check=True)
 negative=subprocess.run([str(binary)],capture_output=True,text=True)
 assert negative.returncode and 'h.inbox_n' in negative.stderr, 'retained complete-request negative did not expose intended duplicate'
 print('retained fully handled request negative refused as expected')
print('raft pending reads: bounded headers, core refusal/retry, retained FIFO/credits, single note and no inbox lock across core PASS')
