#!/usr/bin/env python3
"""Actual resolver ownership under hung NSS, bounded admission and late completion."""
from pathlib import Path
import os,subprocess,tempfile,sys
if sys.platform != "linux":
    print("resolver ownership: SKIP (Linux network implementation)")
    raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
s=(root/'src/common/network.c').read_text();prefix=s[:s.index('int efs_listen_tcp(')]
code=r'''
#include <assert.h>
#include <pthread.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <dirent.h>
static pthread_mutex_t fake_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_cv=PTHREAD_COND_INITIALIZER;
static int frozen=1,entered,create_failure;
static int fake_getaddrinfo(const char *host,const char *service,const struct addrinfo *hints,struct addrinfo **out)
{
 (void)host;(void)hints;
 pthread_mutex_lock(&fake_mu);entered++;
 while(frozen)pthread_cond_wait(&fake_cv,&fake_mu);
 pthread_mutex_unlock(&fake_mu);
 struct addrinfo *r=calloc(1,sizeof(*r));assert(r);
 struct sockaddr_in *a=calloc(1,sizeof(*a));assert(a);
 a->sin_family=AF_INET;a->sin_port=htons(atoi(service));a->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 r->ai_addr=(struct sockaddr *)a;r->ai_addrlen=sizeof(*a);*out=r;return 0;
}
static void fake_freeaddrinfo(struct addrinfo *r) {free(r->ai_addr);free(r);}
static int fake_create(pthread_t *t,const pthread_attr_t *a,void *(*worker)(void *),void *arg)
{if(create_failure) {create_failure=0;return EAGAIN;} return pthread_create(t,a,worker,arg);}
#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo
#define pthread_create fake_create
'''+prefix+r'''
#undef getaddrinfo
#undef freeaddrinfo
#undef pthread_create
int efs_tcp_keepalive(int fd) {(void)fd;return 0;}
static unsigned descriptors(void)
{DIR *d=opendir("/proc/self/fd");assert(d);unsigned n=0;while(readdir(d))n++;closedir(d);return n;}
int main(void)
{
 unsigned baseline=descriptors();struct sockaddr_in address;
 for(unsigned i=0;i<EFS_RESOLVER_JOBS;i++) {
  uint64_t start=resolver_now_ms();efs_net_set_deadline_ms(start+20);
  assert(resolve_ipv4_deadline("hung.test",1234,&address)==-1 && errno==ETIMEDOUT);
  assert(resolver_now_ms()-start<500);
 }
 assert(__atomic_load_n(&resolver_jobs,__ATOMIC_ACQUIRE)==EFS_RESOLVER_JOBS);
 efs_net_set_deadline_ms(resolver_now_ms()+1000);
 assert(resolve_ipv4_deadline("full.test",1234,&address)==-1 && errno==EAGAIN);
 pthread_mutex_lock(&fake_mu);assert(entered==1);frozen=0;pthread_cond_broadcast(&fake_cv);pthread_mutex_unlock(&fake_mu);
 uint64_t end=resolver_now_ms()+2000;
 while(__atomic_load_n(&resolver_jobs,__ATOMIC_ACQUIRE) && resolver_now_ms()<end)usleep(1000);
 assert(!__atomic_load_n(&resolver_jobs,__ATOMIC_ACQUIRE) && descriptors()==baseline);
 efs_net_set_deadline_ms(resolver_now_ms());
 assert(resolve_ipv4_deadline("expired.test",1234,&address)==-1 && errno==ETIMEDOUT);
 assert(!resolver_jobs);
 efs_net_set_deadline_ms(resolver_now_ms()+1000);create_failure=1;
 assert(resolve_ipv4_deadline("create-fail.test",1234,&address)==-1 && errno==EAGAIN);
 assert(!resolver_jobs && descriptors()==baseline);
 int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
 struct sockaddr_in local={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 assert(!bind(listener,(struct sockaddr *)&local,sizeof(local)) && !listen(listener,1));
 socklen_t len=sizeof(local);assert(!getsockname(listener,(struct sockaddr *)&local,&len));
 int client=efs_connect_tcp("ready.test",ntohs(local.sin_port));assert(client>=0);close(client);close(listener);
 end=resolver_now_ms()+2000;
 while(__atomic_load_n(&resolver_jobs,__ATOMIC_ACQUIRE) && resolver_now_ms()<end)usleep(1000);
 assert(!resolver_jobs && descriptors()==baseline);
 puts("resolver: hung NSS deadlines, eight-job cap, late completion ownership, thread failure and hostname connect PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-resolver-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('EFS_TEST_SANITIZE') else []
 subprocess.run(['cc','-D_GNU_SOURCE','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),*flags,str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
