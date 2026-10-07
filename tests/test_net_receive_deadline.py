#!/usr/bin/env python3
"""Actual network receive stops slow-drip/idle peers at the absolute RPC budget."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/common/network.c').read_text();r=(root/'src/client/inode_rpc.c').read_text()
a=s.index('static __thread uint64_t net_deadline_ms;');b=s.index('static void efs_ignore_sigpipe_once(',a)
def fn(text,prefix):
 a=text.index(prefix);return text[a:text.index('\n}',a)+2]
code=r'''
#include "efs/network.h"
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
'''+s[a:b]+fn(s,'int efs_recv_all(')+fn(r,'void efs_client_rpc_set_deadline_ms(')+fn(r,'uint64_t efs_client_rpc_deadline_ms(')+fn(r,'static int rpc_past_deadline(')+r'''
static uint64_t now_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void *drip(void *p) {int fd=*(int *)p;assert(!efs_net_deadline_ms());for(int i=0;i<40;i++){assert(send(fd,"x",1,0)==1);usleep(4000);}return NULL;}
int main(void) {
 int f[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,f));char buf[40];
 int flags=fcntl(f[0],F_GETFL);pthread_t t;assert(!pthread_create(&t,NULL,drip,&f[1]));
 uint64_t start=now_ms();efs_client_rpc_set_deadline_ms(start+20);
 assert(efs_client_rpc_deadline_ms()==efs_net_deadline_ms());
 assert(efs_recv_all(f[0],buf,sizeof(buf))==-1&&errno==ETIMEDOUT&&now_ms()-start<150);
 assert(rpc_past_deadline()&&fcntl(f[0],F_GETFL)==flags);
 assert(!pthread_join(t,NULL));close(f[0]);close(f[1]);
 assert(!socketpair(AF_UNIX,SOCK_STREAM,0,f));start=now_ms();efs_net_set_deadline_ms(start+10);
 assert(efs_recv_all(f[0],buf,1)==-1&&errno==ETIMEDOUT&&now_ms()-start<150);
 assert(send(f[1],"z",1,0)==1);assert(efs_recv_all(f[0],buf,1)==-1&&errno==ETIMEDOUT);
 efs_client_rpc_set_deadline_ms(0);assert(!efs_client_rpc_deadline_ms()&&!rpc_past_deadline());
 assert(!efs_recv_all(f[0],buf,1)&&buf[0]=='z');
 assert(efs_net_remaining_ms(-1)==-1&&efs_net_remaining_ms(30)==30);
 efs_net_set_deadline_ms(UINT64_MAX);assert(efs_net_remaining_ms(-1)==INT_MAX&&efs_net_remaining_ms(5)==5);
 efs_net_set_deadline_ms(0);close(f[0]);close(f[1]);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-net-recv-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('network receive: slow-drip and idle deadlines, RPC binding, TLS isolation and fd flag preservation PASS')
