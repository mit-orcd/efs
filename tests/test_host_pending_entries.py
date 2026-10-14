#!/usr/bin/env python3
"""Actual host enqueue refusal preserves ordered work with flow control off."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
def fn(name):
    starts=[f'static void {name}(',f'static int {name}(']
    a=next(s.index(x) for x in starts if x in s)
    return s[a:s.index('\n}',a)+2]
def struct(name):
    a=s.index('struct '+name+' {');return s[a:s.index('\n};',a)+3]
code=r'''
#define _GNU_SOURCE
#include "efs/raft.h"
#include "efs/raft_flow.h"
#include "efs/wire.h"
#include "efs/batch_obs.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef BO_ON
#undef BO_COUNT
#undef BO_HIST
#define BO_ON() 0
#define BO_COUNT(k) ((void)0)
#define BO_HIST(k,v) ((void)(v))
#define HOST_NGROUPS 2
#define HOST_OUTBOX_MAX 16
#define HOST_OUTBOX_BYTES (1u<<20)
#define HOST_ENCODE_STACK 1024
uint64_t efs_batch_obs_now_us(void){return 1;}
static uint64_t rd64be(const uint8_t*p){return efs_raft_flow_rd64(p);}
static void wr32be(uint8_t*p,uint32_t n){for(int i=3;i>=0;i--){p[i]=n;n>>=8;}}
static void wr64be(uint8_t*p,uint64_t n){for(int i=7;i>=0;i--){p[i]=n;n>>=8;}}
'''+struct('host_outbox_item')+'\n'+struct('host_outbox')+r'''
struct efs_raft_host {int raft_id,tx_running,flow_enabled;pthread_mutex_t outbox_mu;struct host_outbox tx[7];};
static void *host_sender(void*p){return p;}
static int efsd_pthread_create(pthread_t*t,void*(*f)(void*),void*p){(void)t;(void)f;(void)p;return 1;}
int efs_wire_raft_encode(const struct efs_raft_msg*m,uint8_t*b,uint32_t c,uint32_t*n){(void)m;(void)b;(void)c;(void)n;abort();}
'''+ '\n'.join(fn(n) for n in ['outbox_nentries','outbox_entry_lane','outbox_is_heartbeat','outbox_is_ae_rep','outbox_pure_read_control','outbox_pri_replace','host_fill_ae_hdr','host_wire_taken','host_send_ready','host_container_read_probe','host_send','host_sender_take'])+r'''
static struct efs_raft_msg message(int type,int group,int peer){
 struct efs_raft_msg m={0};m.type=type;m.group=group;m.to=peer;m.term=7;m.boot_id=9;m.prev_index=10;
 m.nentries=type==EFS_RAFT_MSG_AE_REQ?1:0;m.wire_len=m.nentries?93:80;m.wire=calloc(1,m.wire_len);assert(m.wire);m.wire[m.wire_len-1]='x';return m;
}
static int send(struct efs_raft_host*h,int type,int group,int peer){struct efs_raft_msg m=message(type,group,peer);int rc=host_send(h,&m);assert(!m.wire);return rc;}
int main(void){
 for(int mode=0;mode<3;mode++){
  struct efs_raft_host*h=calloc(1,sizeof(*h));assert(h);pthread_mutex_init(&h->outbox_mu,NULL);h->tx_running=1;h->flow_enabled=mode!=0;
  for(int p=0;p<7;p++){h->tx[p].started=1;h->tx[p].flow.active=mode==2;pthread_cond_init(&h->tx[p].cv,NULL);}
  struct host_outbox*tx=&h->tx[1];
  assert(send(h,EFS_RAFT_MSG_AE_REQ,0,1)==EFS_OK);
  uint8_t saved[93];memcpy(saved,tx->ent[0].buf,93);
  assert(send(h,EFS_RAFT_MSG_AE_REQ,0,1)==EFS_ERR_AGAIN);
  assert(tx->nent==1&&tx->pending_entries[0]==1&&!memcmp(saved,tx->ent[0].buf,93));
  assert(!host_send_ready(h,1,0)&&host_send_ready(h,1,2));
  assert(send(h,EFS_RAFT_MSG_AE_REQ,2,1)==EFS_OK&&send(h,EFS_RAFT_MSG_AE_REQ,0,2)==EFS_OK);
  assert(send(h,EFS_RAFT_MSG_SNAP_REQ,0,1)==EFS_ERR_AGAIN);
  struct efs_raft_msg probe=message(EFS_RAFT_MSG_AE_REQ,0,1);probe.nentries=0;probe.wire_len=80;
  assert(host_send(h,&probe)==EFS_OK&&!probe.wire); /* read/commit control is not data */
  assert(send(h,EFS_RAFT_MSG_AE_REP,0,1)==EFS_OK&&send(h,EFS_RAFT_MSG_VOTE_REQ,0,1)==EFS_OK);
  uint8_t*b;uint32_t len;host_sender_take(tx,0,0,&b,&len);free(b);
  assert(host_send_ready(h,1,0)&&!host_send_ready(h,1,2));
  assert(send(h,EFS_RAFT_MSG_SNAP_REQ,0,1)==EFS_OK&&tx->pending_entries[0]==1);
  /* Failed thread creation rolls back both accounting and transferred wire. */
  h->tx[3].started=0;assert(send(h,EFS_RAFT_MSG_AE_REQ,0,3)==EFS_ERR_AGAIN);
  assert(!h->tx[3].nent&&!h->tx[3].pending_entries[0]&&!h->tx[3].queued_bytes);
  h->tx_running=0;assert(send(h,EFS_RAFT_MSG_AE_REQ,0,4)==EFS_ERR_AGAIN);
  for(int p=0;p<7;p++){for(int i=0;i<h->tx[p].nent;i++)free(h->tx[p].ent[i].buf);for(int i=0;i<h->tx[p].npri;i++)free(h->tx[p].pri[i].buf);pthread_cond_destroy(&h->tx[p].cv);}
  pthread_mutex_destroy(&h->outbox_mu);free(h);
 }
 puts("host pending entries: default/inactive/active guard, independent groups/peers, control escape, snapshot FIFO and failed admission PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-host-pending-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-pthread','-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
assert 'cfg.send_ready = host_send_ready;' in s, 'default host must install the pre-build guard'
