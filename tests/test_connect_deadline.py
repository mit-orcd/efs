#!/usr/bin/env python3
"""Production connect uses one budget across interrupts and preserves failures."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/common/network.c').read_text()
a=s.index('static int connect_sockaddr(');b=s.index('\nint efs_connect_tcp(',a)
code=r'''
#include "efs/network.h"
#include <stdint.h>
#include <limits.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#define EFS_CONNECT_TIMEOUT_SEC 2
static uint64_t now,deadline;
static int mode,closed,polls,sockets,last_ms;
uint64_t efs_net_deadline_ms(void){return deadline;}
int efs_net_remaining_ms(int cap){return !deadline?cap:now>=deadline?0:(deadline-now<(unsigned)cap?(int)(deadline-now):cap);}
static int fake_clock(int clock,struct timespec *t){(void)clock;t->tv_sec=now/1000;t->tv_nsec=(now%1000)*1000000;return 0;}
static int fake_socket(int a,int b,int c){(void)a;(void)b;(void)c;sockets++;return 1234;}
static int fake_setsockopt(int a,int b,int c,const void*d,socklen_t e){(void)a;(void)b;(void)c;(void)d;(void)e;return 0;}
int efs_tcp_keepalive(int fd){(void)fd;return 0;}
static int fake_fcntl(int fd,int cmd,...){(void)fd;if(cmd==F_GETFL)return 0;if(mode==3&&polls){errno=EIO;return -1;}return 0;}
static int fake_connect(int fd,const struct sockaddr*a,socklen_t n){(void)fd;(void)a;(void)n;if(mode==4){now+=50;return 0;}errno=EINPROGRESS;return -1;}
static int fake_poll(struct pollfd*p,nfds_t n,int ms){(void)p;(void)n;last_ms=ms;polls++;if(mode==1){now+=4;errno=EINTR;return -1;}if(mode==2){now+=50;return 1;}return 1;}
static int fake_getsockopt(int a,int b,int c,void*d,socklen_t*e){(void)a;(void)b;(void)c;(void)e;*(int*)d=mode==5?ECONNREFUSED:0;return 0;}
static int fake_close(int fd){assert(fd==1234);closed++;errno=EBADF;return 0;}
#define clock_gettime fake_clock
#define socket fake_socket
#define setsockopt fake_setsockopt
#define fcntl fake_fcntl
#define connect fake_connect
#define poll fake_poll
#define getsockopt fake_getsockopt
#define close fake_close
'''+s[a:b]+r'''
static void reset(int m){mode=m;now=100;deadline=110;closed=polls=sockets=0;}
int main(void){struct sockaddr a={.sa_family=AF_INET};
 reset(0);assert(connect_sockaddr(&a,sizeof(a))==1234&&last_ms==10&&!closed);
 reset(1);assert(connect_sockaddr(&a,sizeof(a))==-1&&errno==ETIMEDOUT&&polls==3&&closed==1&&last_ms==2);
 reset(2);assert(connect_sockaddr(&a,sizeof(a))==-1&&errno==ETIMEDOUT&&closed==1);
 reset(3);assert(connect_sockaddr(&a,sizeof(a))==-1&&errno==EIO&&closed==1);
 reset(4);assert(connect_sockaddr(&a,sizeof(a))==-1&&errno==ETIMEDOUT&&closed==1&&!polls);
 reset(5);assert(connect_sockaddr(&a,sizeof(a))==-1&&errno==ECONNREFUSED&&closed==1);
 reset(0);deadline=100;assert(connect_sockaddr(&a,sizeof(a))==-1&&errno==ETIMEDOUT&&!sockets);
 reset(0);deadline=0;assert(connect_sockaddr(&a,sizeof(a))==1234&&last_ms==2000);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-connect-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('connect: parent deadline, EINTR, late success, flag restoration and errno cleanup PASS')
