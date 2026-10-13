#!/usr/bin/env python3
"""Actual framed receiver: small control reads, bulk path and broken framing."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/common/protocol.c').read_text()
a=s.index('int efs_recv_msg(');fn=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/common.h"
#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
static unsigned receives;
static int efs_recv_all(int fd,void *p,size_t n) {
 receives++;size_t used=0;while(used<n){ssize_t got=recv(fd,(char*)p+used,n-used,0);if(got<=0)return -1;used+=(size_t)got;}return 0;
}
static int efs_wire_frame_check_nlen(uint32_t n) {return n&&n<=65536?EFS_OK:EFS_ERR_PROTO;}
'''+fn+r'''
struct sender {int fd;uint32_t n;};
static void *drip(void *arg) {struct sender *s=arg;uint32_t be=htonl(s->n);assert(write(s->fd,&be,4)==4);for(uint32_t i=0;i<s->n;i++){uint8_t c=i?i%251:42;assert(write(s->fd,&c,1)==1);usleep(10);}return NULL;}
static void check(uint32_t n,int discard,int fragmented) {
 int fd[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));pthread_t t;struct sender sender={fd[1],n};
 if(fragmented)assert(!pthread_create(&t,NULL,drip,&sender));
 else {uint8_t *raw=malloc(n+4);uint32_t be=htonl(n);memcpy(raw,&be,4);for(uint32_t i=0;i<n;i++)raw[4+i]=i?i%251:42;assert(write(fd[1],raw,n+4)==n+4);free(raw);}
 uint8_t type=0;uint32_t len=0;void *p=(void*)1;receives=0;
 assert(efs_recv_msg(fd[0],&type,discard?NULL:&p,&len)==EFS_OK);assert(type==42&&len==n-1);
 assert(receives==(n<=2048?2:3));
 if(!discard){if(n==1)assert(p==NULL);else {for(uint32_t i=1;i<n;i++)assert(((uint8_t*)p)[i-1]==i%251);free(p);}}
 if(fragmented){assert(!pthread_join(t,NULL));}close(fd[0]);close(fd[1]);
}
int main(void) {
 for(uint32_t n=1;n<=2049;n+=(n<10?1:101))check(n,0,0);
 check(2048,0,0);check(2049,0,0);check(2048,1,0);check(2049,1,0);check(127,0,1);
 int fd[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));uint32_t be=htonl(0);assert(write(fd[1],&be,4)==4);uint8_t type;void *p;uint32_t len;
 assert(efs_recv_msg(fd[0],&type,&p,&len)==EFS_ERR_PROTO); /* header-only invalid frame refuses without waiting for a type */
 be=htonl(12);assert(write(fd[1],&be,4)==4);assert(write(fd[1],"x",1)==1);close(fd[1]);assert(efs_recv_msg(fd[0],&type,&p,&len)==EFS_ERR_NET);close(fd[0]);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-small-frame-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('small framed receive: control/bulk boundary, discarded payload, fragmented delivery, invalid header and truncated peer PASS')
