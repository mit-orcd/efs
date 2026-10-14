#!/usr/bin/env python3
"""Progress that satisfies a wait must not poll again; stale wakes are drained."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/common/rdma.c').read_text()
def function(name):
 a=s.index(name);return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <stdint.h>
#define EFS_RDMA_SEND_WAIT_US 5000000
struct messages {int count;};
struct efs_rdma_conn {int broken;unsigned nrecv,n_reaped,n_posted,pr_head,pr_tail;struct messages messages;};
static int progress_calls,pauses,drains,complete;
static int64_t now_us(void){return 1000;}
int efs_net_remaining_ms(int cap){return cap;}
static void efd_disarm(struct efs_rdma_conn*c){(void)c;}
static void pend_drain_efd(struct efs_rdma_conn*c){(void)c;drains++;}
static void *efs_rmsg_rx_peek(struct messages*m){return m->count?m:0;}
static int message_progress(struct efs_rdma_conn*c,unsigned n){(void)n;progress_calls++;if(complete){c->messages.count=1;c->n_reaped=c->n_posted;}return 0;}
static int message_pause(struct efs_rdma_conn*c,int64_t end){(void)c;(void)end;pauses++;return -1;}
'''+function('int efs_rdma_send_quiesce(')+function('int efs_rdma_message_ready(')+function('int efs_rdma_message_wait(')+r'''
int main(void){struct efs_rdma_conn c={.nrecv=4,.n_posted=1};complete=1;
 assert(efs_rdma_message_wait(&c,5)==EFS_OK&&progress_calls==1&&!pauses);
 c.messages.count=0;c.n_reaped=0;assert(!efs_rdma_send_quiesce(&c)&&!pauses&&c.n_reaped==1);
 complete=0;c.messages.count=0;assert(!efs_rdma_message_ready(&c)&&drains==1);
 c.pr_head=1;assert(!efs_rdma_message_ready(&c)&&drains==1);
 c.pr_head=0;assert(efs_rdma_message_wait(&c,5)==EFS_ERR_NET&&pauses==1);
 c.n_reaped=0;assert(efs_rdma_send_quiesce(&c)<0&&pauses==2);
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-rdma-progress-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('RDMA progress: completed reply/send never waits again; stale control wake drained only on empty ring; incomplete wait still fails PASS')
