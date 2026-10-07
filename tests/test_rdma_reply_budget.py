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
int efs_rdma_recv_wait(struct efs_rdma_conn*c,int ms){(void)c;waits++;seen_timeout=ms;if(expire)remaining=0;return wait_result;}
void *efs_rdma_recv_frame(struct efs_rdma_conn*c,uint32_t*n){(void)c;*n=bad_frame?4:6;return frame;}
int efs_rdma_recv_repost(struct efs_rdma_conn*c){(void)c;reposts++;return 0;}
'''+s[a:b]+r'''
static void reset(void){remaining=10;wait_result=EFS_OK;expire=waits=reposts=bad_frame=0;assert(!efs_wire_frame_header(7,1,frame));frame[5]=42;}
int main(void){struct efs_conn c={.recv_timeout_ms=100};uint8_t type=0;const uint8_t*p=NULL;uint32_t n=0;
 reset();assert(!conn_rdma_frame(&c,&type,&p,&n)&&waits==1&&seen_timeout==10&&!reposts&&type==7&&n==1&&*p==42);
 reset();remaining=0;assert(conn_rdma_frame(&c,&type,&p,&n)==EFS_ERR_BUSY&&!waits&&!reposts);
 reset();expire=1;assert(conn_rdma_frame(&c,&type,&p,&n)==EFS_ERR_BUSY&&reposts==1);
 reset();wait_result=EFS_ERR_AGAIN;assert(conn_rdma_frame(&c,&type,&p,&n)==EFS_ERR_AGAIN&&!reposts);
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
print('RDMA wrapper: shared deadline, late-frame repost, TCP fallback and malformed ownership PASS')
