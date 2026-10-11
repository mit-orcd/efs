#define _GNU_SOURCE
/* Test-only LD_PRELOAD: delay one private follower's real durable log barrier.
 * Never skips a sync, changes its return value, or affects other paths. */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static atomic_uint hits, armed;
static void delay_log(int fd)
{
    int saved=errno;
    const char *marker=getenv("EFS_TEST_RAFT_SLOW_FILE");
    char link[64],path[PATH_MAX];
    if (!marker) return;
    snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
    ssize_t n=readlink(link,path,sizeof(path)-1);
    if(n<0) {errno=saved;return;}
    path[n]=0;
    unsigned node=0;
    for(unsigned i=1;i<=4;i++) {
        char suffix[40];snprintf(suffix,sizeof(suffix),"/n%u/mdraft/log/raft.log",i);
        if(strstr(path,suffix)) {node=i;break;}
    }
    if(!node) {errno=saved;return;}
    int selected=0,marker_fd=open(marker,O_RDONLY);
    if(marker_fd>=0) {
        char digit;ssize_t got=read(marker_fd,&digit,1);close(marker_fd);
        if(got==1 && digit>='1' && digit<='4') selected=digit-'0';
    }
    if(selected==(int)node) {
        if(!atomic_exchange(&armed,1))
            fprintf(stderr,"slow-raft-sync armed node=%u delay_us=100000\n",node);
        atomic_fetch_add(&hits,1);
        struct timespec remaining={0,100000000};
        while(nanosleep(&remaining,&remaining)<0 && errno==EINTR) {}
    } else if(atomic_exchange(&armed,0)) {
        fprintf(stderr,"slow-raft-sync healed node=%u delayed_calls=%u\n",node,atomic_load(&hits));
    }
    errno=saved;
}
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int (*real_fsync)(int),(*real_fdatasync)(int);
static void resolve(void)
{
    real_fsync=dlsym(RTLD_NEXT,"fsync");
    real_fdatasync=dlsym(RTLD_NEXT,"fdatasync");
}
int fsync(int fd)
{
    pthread_once(&once,resolve);
    if(!real_fsync) {errno=ENOSYS;return -1;}
    delay_log(fd);return real_fsync(fd);
}
int fdatasync(int fd)
{
    pthread_once(&once,resolve);
    if(!real_fdatasync) {errno=ENOSYS;return -1;}
    delay_log(fd);return real_fdatasync(fd);
}
