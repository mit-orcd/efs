#!/usr/bin/env python3
"""Pump records strict queued read votes without consuming FIFO/credits."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
def part(prefix,end):
 a=s.index(prefix);return s[a:s.index(end,a)+len(end)]
code=r'''
#include "efs/raft.h"
#include "efs/wire.h"
#include <pthread.h>
#include <assert.h>
#include <string.h>
#define HOST_INBOX_MAX 256
'''+part('struct host_inbox_item {','\n};')+r'''
struct efs_raft_host {pthread_mutex_t inbox_mu;int inbox_n;unsigned work_n;uint64_t inbox_bytes,work_bytes;struct host_inbox_item inbox[256];};
static unsigned calls,published,woken;static int refuse,append;
static uint8_t arrival[EFS_WIRE_RAFT_HDR_LEN]={EFS_RAFT_MSG_AE_REP,1,1};
static struct efs_raft_host *host;
int efs_wire_raft_decode(const uint8_t *p,uint32_t n,struct efs_raft_msg *m,uint8_t *b,uint32_t z){(void)n;(void)b;(void)z;if(p[3])return EFS_ERR_PROTO;memset(m,0,sizeof(*m));m->type=p[0];m->group=p[1];m->last_log_index=p[2];return EFS_OK;}
static struct efs_raft *group_raft(struct efs_raft_host *h,uint8_t g){(void)h;return (struct efs_raft*)(uintptr_t)(g+1);}
int efs_raft_read_control(struct efs_raft *r,const struct efs_raft_msg *m){assert((uintptr_t)r==m->group+1u);calls++;if(append){host->inbox[host->inbox_n++]=(struct host_inbox_item){.buf=arrival,.len=sizeof(arrival)};append=0;}return refuse?0:m->type==EFS_RAFT_MSG_AE_REQ?2:1;}
static void host_publish_group(struct efs_raft_host *h,uint8_t g){(void)h;(void)g;published++;}
static void host_waiters_wake(struct efs_raft_host *h){(void)h;woken++;}
'''+part('static void host_inbox_read_votes(','\n}')+r'''
int main(void){
 struct efs_raft_host h={0};host=&h;pthread_mutex_init(&h.inbox_mu,NULL);h.inbox_n=256;h.work_n=256;h.work_bytes=h.inbox_bytes=256*EFS_WIRE_RAFT_HDR_LEN;
 uint8_t frames[256][EFS_WIRE_RAFT_HDR_LEN];memset(frames,0,sizeof(frames));
 for(unsigned i=0;i<256;i++){frames[i][0]=i%2?EFS_RAFT_MSG_AE_REP:EFS_RAFT_MSG_AE_REQ;frames[i][1]=i%2;frames[i][2]=1;h.inbox[i]=(struct host_inbox_item){.buf=frames[i],.len=sizeof(frames[i]),.queued_us=i};}
 frames[0][0]=EFS_RAFT_MSG_VOTE_REQ;frames[1][2]=0;frames[2][3]=1;h.inbox[3].len--;
 host_inbox_read_votes(&h);assert(calls==252&&published==126&&woken==1);assert(h.inbox_n==256&&h.work_n==256&&h.work_bytes==h.inbox_bytes);
 for(unsigned i=0;i<256;i++){assert(h.inbox[i].buf==frames[i]&&h.inbox[i].queued_us==i);assert(h.inbox[i].read_noted==(i>=4));}
 host_inbox_read_votes(&h);assert(calls==252&&published==126&&woken==1); /* no duplicate early votes next cycle */
 h.inbox_n=1;frames[0][0]=EFS_RAFT_MSG_AE_REP;refuse=1;host_inbox_read_votes(&h);assert(calls==253&&!h.inbox[0].read_noted);
 refuse=0;append=1;host_inbox_read_votes(&h);assert(calls==254&&h.inbox_n==2&&h.inbox[0].read_noted&&!h.inbox[1].read_noted);
 host_inbox_read_votes(&h);assert(calls==255&&h.inbox[1].read_noted);pthread_mutex_destroy(&h.inbox_mu);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-inbox-votes-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('queued read votes: bounded scan, strict refusal, once-only notes, FIFO/credit preservation and concurrent append PASS')
