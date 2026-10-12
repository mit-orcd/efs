#!/usr/bin/env python3
"""Early container inbox votes retain ordered receive and credit ownership."""
from pathlib import Path
import os, subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('static void host_container_pending_reads(');b=s.index('\n}',a)+2
code=r'''
#include "efs/raft.h"
#include "efs/wire.h"
#include "efs/batch_obs.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
#define HOST_INBOX_MAX 16
#define HOST_NGROUPS 2
int efs_batch_obs_on;
void efs_batch_obs_count(enum efs_batch_counter c){(void)c;}
struct server{int storage_engine;};
struct host_inbox_item{uint8_t*buf;uint32_t len;uint64_t queued_us;int read_noted;};
struct efs_raft_host{struct server*s;pthread_mutex_t inbox_mu;int inbox_n;uint64_t inbox_bytes,inbox_work_n,inbox_work_bytes;struct host_inbox_item inbox[HOST_INBOX_MAX];struct{uint8_t group;}g[2];};
static struct efs_raft_host *current;
static int calls,decoded,published,woken,expected_result;
static uint64_t rd64be(const uint8_t*p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
int efs_wire_raft_decode(const uint8_t*p,uint32_t n,struct efs_raft_msg*m,uint8_t*c,uint32_t z){(void)n;(void)c;(void)z;decoded++;if(p[2]==255)return EFS_ERR_PROTO;memset(m,0,sizeof(*m));m->type=p[0];m->group=p[1];m->last_log_index=rd64be(p+28);expected_result=p[3];return EFS_OK;}
static struct efs_raft*group_raft(struct efs_raft_host*h,uint8_t g){(void)h;return (struct efs_raft*)(uintptr_t)(g+1);}
int efs_raft_read_control(struct efs_raft*r,const struct efs_raft_msg*m){(void)r;(void)m;assert(!pthread_mutex_trylock(&current->inbox_mu));pthread_mutex_unlock(&current->inbox_mu);calls++;return expected_result;}
static void host_publish_group(struct efs_raft_host*h,uint8_t g){assert(h==current&&g==2);published++;}
static void host_waiters_wake(struct efs_raft_host*h){assert(h==current);woken++;}
'''+s[a:b]+r'''
int main(void){
 struct server s={0};struct efs_raft_host h={.s=&s,.inbox_n=7,.inbox_bytes=572,.inbox_work_n=7,.inbox_work_bytes=572};current=&h;h.g[1].group=2;
 assert(!pthread_mutex_init(&h.inbox_mu,NULL));uint8_t p[HOST_INBOX_MAX][92]={{0}},saved[HOST_INBOX_MAX][92];
 for(int i=0;i<7;i++){p[i][0]=EFS_RAFT_MSG_AE_REQ;p[i][3]=1;p[i][35]=7;h.inbox[i]=(struct host_inbox_item){p[i],80,(uint64_t)i,0};}
 p[1][0]=EFS_RAFT_MSG_AE_REP;p[1][1]=2;p[1][3]=2;
 p[2][0]=EFS_RAFT_MSG_VOTE_REQ; /* never accelerate term/config controls */
 h.inbox[3].len=92; /* data remains ordered even with a read context */
 p[4][35]=0; /* zero-context heartbeat */
 p[5][2]=255; /* decoder failure */
 p[6][3]=0; /* core refuses: e.g. prefix/term/boot not yet eligible */
 memcpy(saved,p,sizeof(p));
 host_container_pending_reads(&h);assert(!calls&&!decoded&&!published&&!woken);s.storage_engine=1;
 host_container_pending_reads(&h);assert(calls==3&&decoded==4&&published==1&&woken==1);
 assert(h.inbox[0].read_noted&&h.inbox[1].read_noted&&!h.inbox[6].read_noted);
 assert(h.inbox_n==7&&h.inbox_bytes==572&&h.inbox_work_n==7&&h.inbox_work_bytes==572);
 assert(!memcmp(p,saved,sizeof(p)));
 for(int i=0;i<7;i++)assert(h.inbox[i].buf==p[i]&&h.inbox[i].queued_us==(uint64_t)i);
 host_container_pending_reads(&h);assert(calls==4&&published==1&&woken==1); /* accepted votes do not repeat while retained */
 p[6][3]=1;host_container_pending_reads(&h);assert(calls==5&&h.inbox[6].read_noted&&published==1&&woken==1);
 /* A fresh enqueue at a reused slot has no inherited note. */
 h.inbox[0].read_noted=0;p[0][35]=8;host_container_pending_reads(&h);assert(calls==6&&h.inbox[0].read_noted);
 h.inbox_n=HOST_INBOX_MAX;calls=decoded=published=woken=0;
 for(int i=0;i<HOST_INBOX_MAX;i++){
  memset(p[i],0,sizeof(p[i]));p[i][0]=EFS_RAFT_MSG_AE_REQ;p[i][3]=1;p[i][35]=9;
  h.inbox[i]=(struct host_inbox_item){p[i],80,(uint64_t)i,0};
 }
 host_container_pending_reads(&h);assert(calls==HOST_INBOX_MAX&&decoded==HOST_INBOX_MAX&&!published&&!woken);
 for(int i=0;i<HOST_INBOX_MAX;i++)assert(h.inbox[i].read_noted&&h.inbox[i].buf==p[i]);
 assert(h.inbox_n==HOST_INBOX_MAX&&h.inbox_work_n==7&&h.inbox_work_bytes==572);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-container-pending-read-') as d:
 p=Path(d)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-lpthread','-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('container pending reads: bounded headers, core refusal/retry, retained FIFO/credits, single note and no inbox lock across core PASS')
