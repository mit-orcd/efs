#!/usr/bin/env python3
"""Compile the production setup dispatch: no raw TCP after an upgrade."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
s = (root / 'src/server/handler.c').read_text()
a = s.index('        case EFS_MSG_RDMA_SETUP: {')
b = s.index('            if (efs_rdma_trace_on())', a)
case = s[a:b] + 'break; }'
code = r'''
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/rdma.h"
#include "efs/rdma_message.h"
#include <stdlib.h>
#include <assert.h>
static int setup, sent, released;
int efs_rdma_server_accept(struct efs_conn *c,const void *p,uint32_t n,void *r,uint32_t *len)
{ (void)p;(void)n;(void)r;setup++;c->rc=(struct efs_rdma_conn *)1;*len=72;return 0; }
int efs_send_msg(int fd,uint8_t t,const void *p,uint32_t n)
{ (void)fd;(void)t;(void)p;assert(n==72);sent++;return 0; }
void efs_rdma_message_release(struct efs_rdma_conn *c) {assert(c);released++;}
static void dispatch(struct efs_conn *conn,int chan,int rdma_frame)
{
 void *payload=NULL,*to_free=NULL;uint32_t payload_len=0;
 switch(EFS_MSG_RDMA_SETUP) {
''' + case + r'''
 }
}
int main(void)
{
 struct efs_conn c={0};dispatch(&c,EFS_CONN_TCP,0);assert(setup==1 && sent==1 && !released);
 setup=sent=released=0;dispatch(&c,EFS_CONN_RDMA,1);assert(!setup && !sent && released==1);
 setup=sent=released=0;dispatch(&c,EFS_CONN_TCP,0);assert(!setup && !sent && !released);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-rdma-reentry-') as d:
    p = Path(d) / 'test.c'
    p.write_text(code)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'include'), str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True, timeout=5)
print('RDMA setup dispatch: bootstrap allowed; upgraded TCP/RDMA reentry emits no raw TCP PASS')
