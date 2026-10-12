#ifndef EFS_QUERY_STAGE_TRACE_H
#define EFS_QUERY_STAGE_TRACE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
/* Opt-in bounded stage records. Numeric counters only: no names or values.
 * Each event belongs to an existing bounded capture/build attempt. */
static inline int efs_query_stage_enabled(void)
{
    const char *enabled=getenv("EFS_QUERY_STAGE_TRACE");
    return enabled && !strcmp(enabled,"1");
}
static inline void efs_query_stage_trace(const char *stage,int rc,uint64_t elapsed,
                                         uint64_t before,uint64_t after)
{
    if(efs_query_stage_enabled())
        fprintf(stderr,"query-stage: stage=%s rc=%d elapsed_ms=%llu before=%llu after=%llu\n",
            stage,rc,(unsigned long long)elapsed,(unsigned long long)before,
            (unsigned long long)after);
}
static inline void efs_query_root_stage_trace(const char *stage,int rc,uint64_t elapsed,
    uint64_t root,uint64_t generation,uint64_t inode)
{
    if(efs_query_stage_enabled())
        fprintf(stderr,"query-stage: stage=%s rc=%d elapsed_ms=%llu before=%llu after=%llu inode=%llu\n",
            stage,rc,(unsigned long long)elapsed,(unsigned long long)root,
            (unsigned long long)generation,(unsigned long long)inode);
}


/* One fixed-size record per admitted background attempt, max 512 attempts per
 * process. Missing/full observers drop records; diagnostic output is never a
 * permission, publication or scheduling input. Call only after host/query locks. */
struct efs_query_capture_work {
    unsigned pins,pins_ok,pairs,changed,fast;
    uint64_t capture_ms,spacing_ms,dirty,build_ms,checkpoint_ms,total_ms;
    int capture_rc;
    const char *reason;
};
static inline int efs_query_work_enabled(void)
{
    const char *path=getenv("EFS_QUERY_WORK_SOCKET");
    return path && *path;
}
static inline void efs_query_capture_work_trace(int node,int rc,const struct efs_query_capture_work *w)
{
    static unsigned attempted;
    const char *path=getenv("EFS_QUERY_WORK_SOCKET");
    if(!path || !*path || strlen(path)>=sizeof(((struct sockaddr_un *)0)->sun_path))return;
    unsigned seq=__atomic_load_n(&attempted,__ATOMIC_RELAXED);
    do {if(seq>=512)return;} while(!__atomic_compare_exchange_n(&attempted,&seq,seq+1,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
    seq++;
    char line[512];
    int n=snprintf(line,sizeof(line),"query-work: node=%d pid=%ld seq=%u rc=%d reason=%s capture_ms=%llu changed=%u fast=%u spacing_ms=%llu pins=%u pins_ok=%u pairs=%u dirty=%llu build_ms=%llu checkpoint_ms=%llu total_ms=%llu capture_rc=%d\n",
        node,(long)getpid(),seq,rc,w->reason?w->reason:"unknown",
        (unsigned long long)w->capture_ms,w->changed,w->fast,
        (unsigned long long)w->spacing_ms,w->pins,w->pins_ok,w->pairs,
        (unsigned long long)w->dirty,(unsigned long long)w->build_ms,
        (unsigned long long)w->checkpoint_ms,(unsigned long long)w->total_ms,w->capture_rc);
    if(n<0 || (size_t)n>=sizeof(line))return;
    int fd=socket(AF_UNIX,SOCK_DGRAM,0);
    if(fd<0)return;
    if(fcntl(fd,F_SETFL,O_NONBLOCK)<0 || fcntl(fd,F_SETFD,FD_CLOEXEC)<0){close(fd);return;}
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;
    memcpy(address.sun_path,path,strlen(path)+1);
    (void)sendto(fd,line,(size_t)n,MSG_DONTWAIT,(const struct sockaddr *)&address,sizeof(address));
    close(fd);
}

#endif
