#ifndef EFS_STOP_CONTROL_H
#define EFS_STOP_CONTROL_H
/* Private local control channel: quiesce mutations before draining, keeping
 * the serving mount alive on failure. Callback lifetime lasts through join. */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#ifndef EFS_STOP_MS
#define EFS_STOP_MS 60000u
#endif
static struct {
    pthread_rwlock_t gate;
    pthread_t server, worker;
    int listener, started, worker_started, prepared;
    int quit, quiesce, done, result;
    uint64_t deadline;
    int (*drain)(uint64_t, int);
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    /* Stop record (A11 finding 1): the endpoint path with ".stop" for
     * ".sock". It outlives the endpoint: the drain closes the socket
     * before the unmount flushes, settles and revokes. */
    pthread_mutex_t status_mu;
    char status[sizeof(((struct sockaddr_un *)0)->sun_path)];
    char status_phase[24];
    uint64_t status_step, status_ms;
} g_stop = {.gate=PTHREAD_RWLOCK_INITIALIZER, .listener=-1,
            .status_mu=PTHREAD_MUTEX_INITIALIZER};

static uint64_t efs_stop_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
static int efs_stop_path(const char *mount, char *out, size_t len)
{
    /* Never stat/readlink the mounted path: startup has not entered the
     * FUSE loop yet, and an unresponsive mount must still be controllable.
     * Use the absolute mount pathname, with lexical dot/slash cleanup. */
    char absolute[PATH_MAX], canonical[PATH_MAX], cwd[PATH_MAX];
    if (mount[0]=='/') {
        if (snprintf(absolute,sizeof(absolute),"%s",mount)>=(int)sizeof(absolute))
            return -1;
    } else {
        if (!getcwd(cwd,sizeof(cwd)) ||
            snprintf(absolute,sizeof(absolute),"%s/%s",cwd,mount)>=(int)sizeof(absolute))
            return -1;
    }
    size_t used=1;
    canonical[0]='/';
    for (const char *p=absolute;*p;) {
        while(*p=='/')++p;
        const char *begin=p;
        while(*p && *p!='/')++p;
        size_t count=(size_t)(p-begin);
        if(!count || (count==1 && begin[0]=='.'))
            continue;
        if(count==2 && begin[0]=='.' && begin[1]=='.') {
            while(used>1 && canonical[used-1]!='/')--used;
            if(used>1)--used;
            continue;
        }
        if(used>1)canonical[used++]='/';
        memcpy(canonical+used,begin,count);used+=count;
    }
    canonical[used]=0;
    const char *name=canonical;
    uint64_t hash=14695981039346656037ull;
    for (const unsigned char *p=(const unsigned char *)name;*p;p++)
        hash=(hash^*p)*1099511628211ull;
    return snprintf(out,len,"/tmp/efs-control-%016llx.sock",
                     (unsigned long long)hash)>=(int)len ? -1 : 0;
}
/* One line, replaced by rename so a reader sees a whole record:
 *   pid=P phase=NAME step=N total=T rc=R reason=TEXT
 * Phases: serving, flushing, draining, settling, revoking (step grows with
 * every shard), then done (rc=0) or failed (rc and reason). A new
 * (phase, step) is progress; the stop tooling waits while it advances.
 * Writes within one phase are kept to ten a second. */
static void efs_stop_status(const char *phase,uint64_t step,uint64_t total,
                            int rc,const char *reason)
{
    pthread_mutex_lock(&g_stop.status_mu);
    uint64_t now=efs_stop_now();
    int final=!strcmp(phase,"done") || !strcmp(phase,"failed");
    if(!g_stop.status[0] || (!final && !strcmp(phase,g_stop.status_phase) &&
                             step!=0 && now<g_stop.status_ms+100)) {
        pthread_mutex_unlock(&g_stop.status_mu);
        return;
    }
    char line[512],tmp[sizeof(g_stop.status)+32];
    int n=snprintf(line,sizeof(line),"pid=%ld phase=%s step=%llu total=%llu rc=%d reason=%s\n",
                   (long)getpid(),phase,(unsigned long long)step,
                   (unsigned long long)total,rc,reason && *reason ? reason : "-");
    if(n<0 || n>=(int)sizeof(line))
        n=(int)sizeof(line)-1,line[n-1]='\n';
    for(int i=0;i<n-1;i++)
        if(line[i]=='\n' || line[i]=='\r')line[i]=' ';
    if(snprintf(tmp,sizeof(tmp),"%s.%ld.tmp",g_stop.status,(long)getpid())<(int)sizeof(tmp)) {
        int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd>=0) {
            int ok=write(fd,line,(size_t)n)==n;
            close(fd);
            if(!ok || rename(tmp,g_stop.status)!=0)
                unlink(tmp);
        }
    }
    snprintf(g_stop.status_phase,sizeof(g_stop.status_phase),"%s",phase);
    g_stop.status_step=step;g_stop.status_ms=now;
    pthread_mutex_unlock(&g_stop.status_mu);
}
static void efs_stop_phase(const char *phase,uint64_t step,uint64_t total)
{
    efs_stop_status(phase,step,total,0,NULL);
}
static int efs_stop_socket(void)
{
    int fd=socket(AF_UNIX,SOCK_STREAM,0);
    if(fd>=0) {
        (void)fcntl(fd,F_SETFD,FD_CLOEXEC);
        struct timeval timeout={1,0};
        (void)setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        (void)setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
#ifdef SO_NOSIGPIPE
        int on=1;
        (void)setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&on,sizeof(on));
#endif
    }
    return fd;
}
static void efs_stop_reply(int fd,const char *text)
{
#ifdef MSG_NOSIGNAL
    (void)send(fd,text,strlen(text),MSG_NOSIGNAL);
#else
    (void)send(fd,text,strlen(text),0);
#endif
}
static int efs_stop_mutation_enter(void)
{
    if (__atomic_load_n(&g_stop.quiesce,__ATOMIC_ACQUIRE))
        return 0;
    pthread_rwlock_rdlock(&g_stop.gate);
    if (__atomic_load_n(&g_stop.quiesce,__ATOMIC_ACQUIRE)) {
        pthread_rwlock_unlock(&g_stop.gate);
        return 0;
    }
    return 1;
}
static void efs_stop_mutation_leave(void)
{
    pthread_rwlock_unlock(&g_stop.gate);
}
static void *efs_stop_worker(void *unused)
{
    (void)unused;
    g_stop.result=g_stop.drain(g_stop.deadline,0);
    __atomic_store_n(&g_stop.done,1,__ATOMIC_RELEASE);
    return NULL;
}
/* Only the server thread owns/releases the exclusive gate. A timed-out
 * drain continues owning its callback state; it can never free live jobs. */
static int efs_stop_prepare(int force)
{
    if(g_stop.prepared)
        return 0;
    if(g_stop.worker_started) {
        if(!__atomic_load_n(&g_stop.done,__ATOMIC_ACQUIRE))
            return -1;
        pthread_join(g_stop.worker,NULL);
        g_stop.worker_started=0;
    }
    uint64_t deadline=efs_stop_now()+EFS_STOP_MS;
    __atomic_store_n(&g_stop.quiesce,1,__ATOMIC_RELEASE);
    while(pthread_rwlock_trywrlock(&g_stop.gate)!=0) {
        if(efs_stop_now()>=deadline) {
            __atomic_store_n(&g_stop.quiesce,0,__ATOMIC_RELEASE);
            return -1;
        }
        usleep(1000);
    }
    int result;
    if(force)
        result=g_stop.drain(deadline,1);
    else {
        g_stop.deadline=deadline;
        __atomic_store_n(&g_stop.done,0,__ATOMIC_RELEASE);
        if(pthread_create(&g_stop.worker,NULL,efs_stop_worker,NULL)!=0)
            result=-1;
        else {
            g_stop.worker_started=1;
            while(!__atomic_load_n(&g_stop.done,__ATOMIC_ACQUIRE) &&
                  efs_stop_now()<deadline)
                usleep(1000);
            result=__atomic_load_n(&g_stop.done,__ATOMIC_ACQUIRE) ?
                        g_stop.result : -1;
        }
    }
    if(result==0) {
        g_stop.prepared=1;
        return 0; /* keep mutations quiesced through unmount */
    }
    pthread_rwlock_unlock(&g_stop.gate);
    __atomic_store_n(&g_stop.quiesce,0,__ATOMIC_RELEASE);
    return -1;
}
static void efs_stop_resume(void)
{
    if(g_stop.prepared) {
        g_stop.prepared=0;
        pthread_rwlock_unlock(&g_stop.gate);
    }
    __atomic_store_n(&g_stop.quiesce,0,__ATOMIC_RELEASE);
}
static void *efs_stop_server(void *unused)
{
    (void)unused;
    while(!__atomic_load_n(&g_stop.quit,__ATOMIC_ACQUIRE)) {
        struct pollfd p={.fd=g_stop.listener,.events=POLLIN};
        if(poll(&p,1,100)<=0)
            continue;
        int fd=accept(g_stop.listener,NULL,NULL);
        if(fd<0)
            continue;
        struct timeval timeout={1,0};
        (void)setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        (void)setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
#ifdef SO_NOSIGPIPE
        int on=1;
        (void)setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&on,sizeof(on));
#endif
        char command[16]={0};
        size_t used=0;
        ssize_t n=0;
        while(used<sizeof(command)-1) {
            n=recv(fd,command+used,sizeof(command)-1-used,0);
            if(n<=0)
                break;
            used+=(size_t)n;
            if(memchr(command,'\n',used))
                break;
        }
        n=(ssize_t)used;
        if(n>0 && strcmp(command,"RESUME\n")==0) {
            efs_stop_resume();efs_stop_reply(fd,"OK\n");
        } else if(n>0 && (strcmp(command,"DRAIN\n")==0 ||
                          strcmp(command,"FORCE\n")==0)) {
            int force=strcmp(command,"FORCE\n")==0;
            efs_stop_reply(fd,efs_stop_prepare(force)==0?"OK\n":"BUSY\n");
        } else
            efs_stop_reply(fd,"INVALID\n");
        close(fd);
    }
    if(g_stop.prepared) {
        g_stop.prepared=0;
        pthread_rwlock_unlock(&g_stop.gate);
    }
    return NULL;
}
static int efs_stop_start(const char *mount,int (*drain)(uint64_t,int))
{
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    if(efs_stop_path(mount,addr.sun_path,sizeof(addr.sun_path)))
        return -1;
    int fd=efs_stop_socket();
    if(fd<0)
        return -1;
    struct stat st;
    if(lstat(addr.sun_path,&st)==0) {
        int probe=efs_stop_socket();
        int active=probe<0 || connect(probe,(struct sockaddr *)&addr,sizeof(addr))==0;
        if(probe>=0)close(probe);
        if(active || !S_ISSOCK(st.st_mode) || st.st_uid!=geteuid()) {
            close(fd);return -1;
        }
        if(unlink(addr.sun_path)!=0) {close(fd);return -1;}
    }
    if(bind(fd,(struct sockaddr *)&addr,sizeof(addr))!=0) {close(fd);return -1;}
    if(chmod(addr.sun_path,0600)!=0 || listen(fd,4)!=0) {
        close(fd);unlink(addr.sun_path);return -1;
    }
    strcpy(g_stop.path,addr.sun_path);
    pthread_mutex_lock(&g_stop.status_mu);
    strcpy(g_stop.status,addr.sun_path);
    strcpy(g_stop.status+strlen(g_stop.status)-5,".stop");
    g_stop.status_phase[0]=0;
    pthread_mutex_unlock(&g_stop.status_mu);
    efs_stop_phase("serving",0,0);
    g_stop.listener=fd;g_stop.drain=drain;
    __atomic_store_n(&g_stop.quiesce,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_stop.quit,0,__ATOMIC_RELEASE);
    if(pthread_create(&g_stop.server,NULL,efs_stop_server,NULL)!=0) {
        close(fd);unlink(addr.sun_path);g_stop.listener=-1;return -1;
    }
    g_stop.started=1;
    return 0;
}
static void efs_stop_finish(void)
{
    if(!g_stop.started)
        return;
    __atomic_store_n(&g_stop.quit,1,__ATOMIC_RELEASE);
    pthread_join(g_stop.server,NULL);
    if(g_stop.worker_started) {
        pthread_join(g_stop.worker,NULL);
        g_stop.worker_started=0;
    }
    close(g_stop.listener);unlink(g_stop.path);
    g_stop.listener=-1;g_stop.started=0;
}
/* Return 3 for unavailable old/dead clients; normal stop must fail closed. */
static int efs_stop_client(const char *mount,const char *command)
{
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    if(efs_stop_path(mount,addr.sun_path,sizeof(addr.sun_path)))
        return 3;
    int fd=efs_stop_socket();
    if(fd<0)
        return 3;
    if(connect(fd,(struct sockaddr *)&addr,sizeof(addr))!=0) {close(fd);return 3;}
    efs_stop_reply(fd,command);
    struct pollfd p={.fd=fd,.events=POLLIN};
    char reply[16]={0};
    int ready=poll(&p,1,EFS_STOP_MS+5000);
    size_t used=0;
    while(ready>0 && used<sizeof(reply)-1) {
        ssize_t part=recv(fd,reply+used,sizeof(reply)-1-used,0);
        if(part<=0)
            break;
        used+=(size_t)part;
        if(memchr(reply,'\n',used))
            break;
    }
    ssize_t n=(ssize_t)used;
    close(fd);
    return n>0&&strcmp(reply,"OK\n")==0?0:1;
}
#endif
