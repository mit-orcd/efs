#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static int (*real_connect)(int,const struct sockaddr*,socklen_t);
static int (*real_accept)(int,struct sockaddr*,socklen_t*);
static int (*real_accept4)(int,struct sockaddr*,socklen_t*,int);
static pthread_once_t once=PTHREAD_ONCE_INIT;
static unsigned count;
static void initialize(void){real_connect=dlsym(RTLD_NEXT,"connect");real_accept=dlsym(RTLD_NEXT,"accept");real_accept4=dlsym(RTLD_NEXT,"accept4");}
static void observe(int fd,const struct sockaddr *addr,const char *kind){
 int saved=errno;struct sockaddr_storage local;socklen_t llen=sizeof(local);
 if(!getenv("EFS_TCP_DIAG"))return;
 if(!addr){if(getsockname(fd,(struct sockaddr*)&local,&llen))goto done;addr=(struct sockaddr*)&local;}
 if(addr->sa_family!=AF_INET)goto done;
 unsigned port=ntohs(((const struct sockaddr_in*)addr)->sin_port);
 if(port<34510 || port>34513)goto done;
 if(__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)>=128)goto done;
 int value=-1;socklen_t size=sizeof(value);int rc=getsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&value,&size);
 struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
 fprintf(stderr,"intel-tcp-diag pid=%ld kind=%s fd=%d port=%u nodelay=%d get_rc=%d mono_ns=%llu\n",(long)getpid(),kind,fd,port,value,rc,(unsigned long long)now.tv_sec*1000000000ULL+now.tv_nsec);
 done:errno=saved;
}
int connect(int fd,const struct sockaddr *addr,socklen_t len){pthread_once(&once,initialize);int rc=real_connect(fd,addr,len);int saved=errno;observe(fd,addr,"outbound");errno=saved;return rc;}
int accept(int fd,struct sockaddr *addr,socklen_t *len){pthread_once(&once,initialize);int out=real_accept(fd,addr,len);int saved=errno;if(out>=0)observe(out,NULL,"accepted");errno=saved;return out;}
int accept4(int fd,struct sockaddr *addr,socklen_t *len,int flags){pthread_once(&once,initialize);int out=real_accept4(fd,addr,len,flags);int saved=errno;if(out>=0)observe(out,NULL,"accepted");errno=saved;return out;}
