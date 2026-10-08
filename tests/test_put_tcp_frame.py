#!/usr/bin/env python3
"""Production TCP PUT frame watcher with real fragmented socket delivery."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/common/protocol.c').read_text()
def function(name):
    a=s.index('int '+name+'(')
    return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
static unsigned noted,quick,spin;
void efs_conn_note_ok(struct efs_conn *c){(void)c;noted++;}
int efs_conn_reply_watch_quick(struct efs_conn *c){assert(c->rc);quick++;return EFS_CONN_REPLY_READY;}
int efs_conn_reply_watch_us(struct efs_conn *c,int b){assert(c->rc && b==24);spin++;return EFS_CONN_REPLY_READY;}
int efs_recv_put_reply(int fd,uint8_t *t,uint8_t *s,uint8_t *p){(void)fd;(void)t;(void)s;(void)p;assert(0);return -1;}
static int conn_rdma_frame(struct efs_conn *c,uint8_t *t,const uint8_t **p,uint32_t *n){(void)c;(void)t;(void)p;(void)n;assert(0);return -1;}
int efs_rdma_recv_repost(struct efs_rdma_conn *c){(void)c;assert(0);return -1;}
'''+function('efs_conn_recv_put_reply')+function('efs_conn_put_reply_watch')+r'''
int main(void){
 int fd[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));struct efs_conn c={.fd=fd[0]};
 for(unsigned size=2;size<=4;size++){
  uint8_t frame[8]={0};uint32_t be=htonl(size);memcpy(frame,&be,4);frame[4]=EFS_MSG_PUT_CHUNK_REPLY;frame[5]=EFS_PUT_CHUNK_OK;frame[6]=7;
  assert(efs_conn_put_reply_watch(&c,0)==fd[0]);
  for(unsigned i=0;i<4+size;i++){
   assert(write(fd[1],frame+i,1)==1);
   int w=efs_conn_put_reply_watch(&c,0);
   assert(w==(i+1==4+size?EFS_CONN_REPLY_READY:fd[0]));
  }
  assert(efs_conn_put_reply_watch(&c,0)==EFS_CONN_REPLY_READY);
  uint8_t type=0,status=1,path=0;
  assert(!efs_conn_recv_put_reply(&c,&type,&status,&path));
  assert(type==EFS_MSG_PUT_CHUNK_REPLY&&status==EFS_PUT_CHUNK_OK&&path==(size>=3?7:0xff)&&!c.put_reply_used);
 }
 assert(noted==3);
 for(unsigned len=0;len<=5;len++){
  if(len>=2&&len<=4)continue;
  uint32_t be=htonl(len);assert(write(fd[1],&be,4)==4);
  assert(efs_conn_put_reply_watch(&c,0)==-1);c.put_reply_used=0;
 }
 uint32_t bad=htonl(UINT32_MAX);assert(write(fd[1],&bad,4)==4);
 assert(efs_conn_put_reply_watch(&c,0)==-1);c.put_reply_used=0;
 uint8_t partial=0;assert(write(fd[1],&partial,1)==1);
 assert(efs_conn_put_reply_watch(&c,0)==fd[0] && c.put_reply_used==1);
 close(fd[1]);assert(efs_conn_put_reply_watch(&c,0)==-1);close(fd[0]);
 c.rc=(void *)1;c.recv_chan=EFS_CONN_RDMA;c.put_reply_used=0;
 assert(efs_conn_put_reply_watch(&c,0)==EFS_CONN_REPLY_READY && quick==1 && !spin);
 assert(efs_conn_put_reply_watch(&c,24)==EFS_CONN_REPLY_READY && quick==1 && spin==1 && !c.put_reply_used);
 puts("PUT TCP frames: bytewise header/body, legacy lengths, reuse, malformed length, partial EOF and RDMA delegation PASS");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-put-frame-') as d:
    p=Path(d)/'t.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
