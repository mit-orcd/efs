#!/usr/bin/env python3
"""Production TCP frame sender bounds blocked and partial sends by the RPC deadline."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/common/network.c').read_text(); p=(root/'src/common/protocol.c').read_text()
a=s.index('static __thread uint64_t net_deadline_ms;');b=s.index('static void efs_ignore_sigpipe_once(',a)
def fn(text,prefix):
 a=text.index(prefix);return text[a:text.index('\n}',a)+2]
code=r'''
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/wire.h"
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/in.h>
'''+s[a:b]+fn(p,'static int send_iov_run(')+fn(p,'static int send_iov(')+fn(p,'int efs_send_msg_parts(')+r'''
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int tcp_pair(int f[2]) {
 int l=socket(AF_INET,SOCK_STREAM,0);assert(l>=0);struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 assert(!bind(l,(void *)&a,sizeof(a))&&!listen(l,1));socklen_t n=sizeof(a);assert(!getsockname(l,(void *)&a,&n));
 f[0]=socket(AF_INET,SOCK_STREAM,0);assert(f[0]>=0&&!connect(f[0],(void *)&a,n));f[1]=accept(l,NULL,NULL);close(l);return f[1]<0?-1:0;
}
int main(void){
 int f[2],small=4096;assert(!tcp_pair(f));
 assert(!setsockopt(f[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small)));
 int flags=fcntl(f[0],F_GETFL);char *body=malloc(1<<20);assert(body);memset(body,'x',1<<20);
 struct iovec v={body,1<<20};uint64_t start=now_ms();efs_net_set_deadline_ms(start+10);
 assert(send_iov(f[0],&v,1)==EFS_ERR_NET&&errno==ETIMEDOUT&&now_ms()-start<150);
 assert(v.iov_len<(1<<20)&&v.iov_len>0&&fcntl(f[0],F_GETFL)==flags);
 start=now_ms();efs_net_set_deadline_ms(start+10);v=(struct iovec){body,1};
 assert(send_iov(f[0],&v,1)==EFS_ERR_NET&&errno==ETIMEDOUT&&now_ms()-start<150);
 close(f[0]);close(f[1]);assert(!tcp_pair(f));
 efs_net_set_deadline_ms(now_ms()-1);assert(efs_send_msg_parts(f[0],7,"a",1,"b",1)==EFS_ERR_NET&&errno==ETIMEDOUT);
 char raw[7];assert(recv(f[1],raw,sizeof(raw),MSG_DONTWAIT)<0&&errno==EAGAIN);
 efs_net_set_deadline_ms(now_ms()+100);assert(efs_send_msg_parts(f[0],7,"a",1,"b",1)==EFS_OK);
 assert(recv(f[1],raw,sizeof(raw),MSG_WAITALL)==7&&raw[4]==7&&raw[5]=='a'&&raw[6]=='b');
 efs_net_set_deadline_ms(0);assert(efs_send_msg_parts(f[0],8,"c",1,"d",1)==EFS_OK);
 assert(recv(f[1],raw,sizeof(raw),MSG_WAITALL)==7&&raw[4]==8&&raw[5]=='c'&&raw[6]=='d');
 free(body);close(f[0]);close(f[1]);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-net-send-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'src/wire/wire.c'),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('network send: partial/full socket deadlines, expired admission, wire framing and fd flags PASS')
