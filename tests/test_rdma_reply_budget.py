#!/usr/bin/env python3
"""Production RDMA reply wrapper respects TCP-shared budget and pool ownership."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/common/protocol.c').read_text();a=s.index('static int conn_rdma_frame(');b=s.index('\n}',a)+2
code=r'''
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include "efs/wire.h"
#include <assert.h>
#include <limits.h>
static int remaining,wait_result,expire,waits,reposts,seen_timeout,bad_frame;
static uint8_t frame[6];
int efs_net_remaining_ms(int cap){return remaining<cap?remaining:cap;}
int efs_rdma_message_wait(struct efs_rdma_conn*c,int ms){(void)c;waits++;seen_timeout=ms;if(expire)remaining=0;return wait_result;}
void *efs_rdma_message_frame(struct efs_rdma_conn*c,uint32_t*n){(void)c;*n=bad_frame?4:6;return frame;}
void efs_rdma_message_release(struct efs_rdma_conn*c){(void)c;reposts++;}
'''+s[a:b]+r'''
static void reset(void){remaining=10;wait_result=EFS_OK;expire=waits=reposts=bad_frame=0;assert(!efs_wire_frame_header(7,1,frame));frame[5]=42;}
int main(void){struct efs_conn c={.recv_timeout_ms=100};uint8_t type=0;const uint8_t*p=NULL;uint32_t n=0;
 reset();assert(!conn_rdma_frame(&c,&type,&p,&n)&&waits==1&&seen_timeout==10&&!reposts&&type==7&&n==1&&*p==42);
 reset();remaining=0;assert(conn_rdma_frame(&c,&type,&p,&n)==EFS_ERR_BUSY&&!waits&&!reposts);
 reset();expire=1;assert(conn_rdma_frame(&c,&type,&p,&n)==EFS_ERR_BUSY&&reposts==1);
 reset();wait_result=EFS_ERR_AGAIN;assert(conn_rdma_frame(&c,&type,&p,&n)==EFS_ERR_NET&&!reposts);
 reset();wait_result=EFS_ERR_AGAIN;expire=1;assert(conn_rdma_frame(&c,&type,&p,&n)==EFS_ERR_BUSY&&!reposts);
 reset();bad_frame=1;assert(conn_rdma_frame(&c,&type,&p,&n)==EFS_ERR_PROTO&&reposts==1);
 reset();remaining=INT_MAX;c.recv_timeout_ms=0;assert(!conn_rdma_frame(&c,&type,&p,&n)&&seen_timeout==EFS_IO_TIMEOUT_MS);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-rdma-budget-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'src/wire/wire.c'),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('RDMA wrapper: shared deadline, late-frame repost, strict no-fallback and malformed ownership PASS')

# Exercise the production send-slot selector without requiring a local HCA.
rdma=(root/'src/common/rdma.c').read_text()
a=rdma.index('static int send_buf_prepare(');b=rdma.index('\n/* Describe a send failure.',a)
slot_code=r'''
#include <assert.h>
#include <stdint.h>
#include <sched.h>
#define EFS_RDMA_NSEND 32
#define EFS_RDMA_SEND_WAIT_US 5000000
#define IBV_ACCESS_LOCAL_WRITE 1
struct ibv_mr {unsigned lkey;};
struct efs_rdma_dev {void *pd;};
struct efs_rdma_conn {
 struct efs_rdma_dev *dev;unsigned bufsz;uint8_t **send_bufs;
 struct ibv_mr *send_mrs[32];int send_busy[32],send_rr,reserved,broken;
};
static struct ibv_mr regions[32];
static int registrations,fail_registration,reap_error,complete_on_reap;
static struct ibv_mr *ibv_reg_mr(void *pd,void *base,unsigned size,int access) {
 assert(pd && base && size==86016 && access==1);
 if(fail_registration)return 0;
 assert(registrations<32);return &regions[registrations++];
}
static int reap_sends(struct efs_rdma_conn *rc) {
 if(reap_error)return -1;
 if(complete_on_reap)rc->send_busy[0]=0;
 return 0;
}
'''+rdma[a:b]+r'''
int main(void) {
 struct efs_rdma_dev dev={.pd=&dev};uint8_t *buffers[32];uint8_t bytes[32];
 for(int i=0;i<32;i++)buffers[i]=&bytes[i];
 struct efs_rdma_conn rc={.dev=&dev,.bufsz=86016,.send_bufs=buffers,.reserved=-1};
 for(int i=0;i<1000;i++)assert(send_buf_pick(&rc)==0);
 assert(registrations==1);

 for(int i=0;i<31;i++) {
  int idx=send_buf_pick(&rc);assert(idx>=0 && !rc.send_busy[idx]);rc.send_busy[idx]=1;
 }
 assert(registrations==31);
 assert(send_buf_pick(&rc)==-2 && !rc.broken);
 rc.broken=0;rc.send_rr=0;complete_on_reap=1;
 assert(send_buf_pick(&rc)==0 && registrations==31);
 reap_error=1;assert(send_buf_pick(&rc)==-1);
 reap_error=complete_on_reap=0;
 struct efs_rdma_conn cold={.dev=&dev,.bufsz=86016,.send_bufs=buffers,.reserved=-1};
 fail_registration=1;assert(send_buf_pick(&cold)==-1 && !cold.send_mrs[0] && !cold.send_busy[0]);
 assert(registrations==31);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-rdma-slots-') as d:
 p=Path(d)/'t.c';p.write_text(slot_code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('RDMA slots: serial reuse, reserved ownership, 31 DATA slots/control reserve, nonblocking saturation and registration refusal PASS')
