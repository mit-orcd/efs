#include "server_internal.h"
#include "efs/checksum.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define PUT_SESSION_STRIPES 64u
#define PUT_SESSION_CERT_BYTES 56u
static pthread_mutex_t put_session_mu[PUT_SESSION_STRIPES];
static pthread_once_t put_session_once=PTHREAD_ONCE_INIT;
static void put_session_init(void)
{
    for(unsigned i=0;i<PUT_SESSION_STRIPES;i++)pthread_mutex_init(&put_session_mu[i],NULL);
}
static int storage_fence_path(struct efsd_server *s,int repair,const uint8_t uuid[16],
                             char path[EFS_MAX_PATH+80],pthread_mutex_t **mu)
{
    if(!s || !uuid || !s->storage_path_count || !s->storage_paths[0][0])return EFS_ERR_INVAL;
    unsigned nonzero=0,hash=0;char hex[33];
    for(unsigned i=0;i<16;i++){nonzero|=uuid[i];hash=hash*33u+uuid[i];snprintf(hex+2*i,3,"%02x",uuid[i]);}
    if(!nonzero)return EFS_ERR_INVAL;
    int n=snprintf(path,EFS_MAX_PATH+80,"%s/meta/%s/%s",s->storage_paths[0],repair?"repair-job":"put-session",hex);
    if(n<0 || n>=EFS_MAX_PATH+80)return EFS_ERR_INVAL;
    pthread_once(&put_session_once,put_session_init);
    *mu=&put_session_mu[hash%PUT_SESSION_STRIPES];return EFS_OK;
}
static int storage_fence_read(const char *path,int repair,const uint8_t uuid[16],uint32_t *floor)
{
    *floor=0;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return errno==ENOENT?EFS_OK:EFS_ERR_IO;
    struct stat st;uint8_t b[PUT_SESSION_CERT_BYTES],sum[EFS_HASH_SIZE];int rc=EFS_OK;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size!=sizeof(b))rc=EFS_ERR_IO;
    size_t off=0;
    while(rc==EFS_OK && off<sizeof(b)){
        ssize_t n=pread(fd,b+off,sizeof(b)-off,(off_t)off);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){rc=EFS_ERR_IO;break;}off+=(size_t)n;
    }
    if(close(fd))rc=EFS_ERR_IO;
    if(rc!=EFS_OK)return rc;
    efs_hash(b,24,sum);
    if(memcmp(b,repair?"ERF1":"EPF1",4) || memcmp(b+4,uuid,16) || memcmp(b+24,sum,32))return EFS_ERR_IO;
    *floor=((uint32_t)b[20]<<24)|((uint32_t)b[21]<<16)|((uint32_t)b[22]<<8)|b[23];
    return *floor?EFS_OK:EFS_ERR_IO;
}
static int put_session_sync_dir(const char *path,int configured_root)
{
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|(configured_root?0:O_NOFOLLOW));
    if(fd<0)return EFS_ERR_IO;
    int rc;do{rc=fsync(fd);}while(rc<0 && errno==EINTR);
    int closed=close(fd);return rc<0 || closed?EFS_ERR_IO:EFS_OK;
}
static int put_session_sync_parents(const char *path)
{
    char dir[EFS_MAX_PATH+80];strcpy(dir,path);
    /* Seal certificate directory, meta directory and the configured root.
     * Repeat even if rename previously succeeded but its directory sync failed. */
    for(unsigned i=0;i<3;i++){
        char *slash=strrchr(dir,'/');if(!slash || slash==dir)return EFS_ERR_IO;
        *slash=0;int rc=put_session_sync_dir(dir,i==2);if(rc!=EFS_OK)return rc;
    }
    return EFS_OK;
}
static int storage_fence_begin(struct efsd_server *s,int repair,const uint8_t uuid[16],
                             uint32_t epoch,struct efs_put_storage_guard *guard)
{
    if(!epoch || !guard || guard->locked)return EFS_ERR_INVAL;
    char path[EFS_MAX_PATH+80];pthread_mutex_t *mu;
    int rc=storage_fence_path(s,repair,uuid,path,&mu);if(rc!=EFS_OK)return rc;
    pthread_mutex_lock(mu);uint32_t floor=0;rc=storage_fence_read(path,repair,uuid,&floor);
    if(rc==EFS_OK && epoch<floor)rc=EFS_ERR_STALE;
    if(rc==EFS_OK)guard->locked=mu;else pthread_mutex_unlock(mu);
    return rc;
}
void server_put_session_end(struct efs_put_storage_guard *guard)
{
    if(guard && guard->locked){pthread_mutex_t *mu=guard->locked;guard->locked=NULL;pthread_mutex_unlock(mu);}
}
static int storage_fence_seal(struct efsd_server *s,int repair,const uint8_t uuid[16],uint32_t floor)
{
    if(!floor)return EFS_ERR_INVAL;
    char path[EFS_MAX_PATH+80];pthread_mutex_t *mu;
    int rc=storage_fence_path(s,repair,uuid,path,&mu);if(rc!=EFS_OK)return rc;
    pthread_mutex_lock(mu);uint32_t old=0;rc=storage_fence_read(path,repair,uuid,&old);
    if(rc==EFS_OK && floor>old){
        char dir[EFS_MAX_PATH+80];strcpy(dir,path);*strrchr(dir,'/')=0;
        char *slash=strrchr(dir,'/');*slash=0;
        if(mkdir(dir,0755) && errno!=EEXIST)rc=EFS_ERR_IO;
        *slash='/';if(rc==EFS_OK && mkdir(dir,0755) && errno!=EEXIST)rc=EFS_ERR_IO;
        /* Refuse symlink directories before creating/replacing a certificate. */
        if(rc==EFS_OK)rc=put_session_sync_dir(dir,0);
        uint8_t b[PUT_SESSION_CERT_BYTES]={0};memcpy(b,repair?"ERF1":"EPF1",4);memcpy(b+4,uuid,16);
        b[20]=(uint8_t)(floor>>24);b[21]=(uint8_t)(floor>>16);b[22]=(uint8_t)(floor>>8);b[23]=(uint8_t)floor;
        efs_hash(b,24,b+24);
        char tmp[EFS_MAX_PATH+96];int n=snprintf(tmp,sizeof(tmp),"%s.tmp.XXXXXX",path);
        int fd=rc==EFS_OK && n>=0 && (size_t)n<sizeof(tmp)?mkstemp(tmp):-1;
        if(fd<0)rc=EFS_ERR_IO;
        else{
            if(fcntl(fd,F_SETFD,FD_CLOEXEC)<0)rc=EFS_ERR_IO;
            size_t off=0;while(rc==EFS_OK && off<sizeof(b)){
                ssize_t wrote=write(fd,b+off,sizeof(b)-off);
                if(wrote<0 && errno==EINTR)continue;
                if(wrote<=0){rc=EFS_ERR_IO;break;}off+=(size_t)wrote;
            }
            int synced;do{synced=fsync(fd);}while(synced<0 && errno==EINTR);
            if(close(fd) || synced<0)rc=EFS_ERR_IO;
            if(rc==EFS_OK && rename(tmp,path))rc=EFS_ERR_IO;
            if(rc!=EFS_OK)unlink(tmp);
        }
    }
    if(rc==EFS_OK)rc=put_session_sync_parents(path);
    pthread_mutex_unlock(mu);return rc;
}

/* Independent certificate domains prevent a client session UUID from sealing
 * a repair job. No receiver/worker is enabled by these local barriers. */
int server_put_session_begin(struct efsd_server *s,const uint8_t uuid[16],uint32_t epoch,
                             struct efs_put_storage_guard *guard)
{return storage_fence_begin(s,0,uuid,epoch,guard);}
int server_put_session_fence(struct efsd_server *s,const uint8_t uuid[16],uint32_t floor)
{return storage_fence_seal(s,0,uuid,floor);}
int server_repair_storage_begin(struct efsd_server *s,const uint8_t uuid[16],uint32_t epoch,
                               struct efs_put_storage_guard *guard)
{return storage_fence_begin(s,1,uuid,epoch,guard);}
int server_repair_storage_seal(struct efsd_server *s,const uint8_t uuid[16],uint32_t floor)
{return storage_fence_seal(s,1,uuid,floor);}

/* Target incarnation is read-only to receivers. Missing/corrupt identity never
 * means a fresh authority; only trusted offline/opt-in storage initialization
 * may create a new random identity. Losing it invalidates every old job. */
static int repair_store_path(struct efsd_server *s,char path[EFS_MAX_PATH+80])
{
    if(!s || s->storage_path_count!=1 || !s->storage_paths[0][0])return EFS_ERR_INVAL;
    int n=snprintf(path,EFS_MAX_PATH+80,"%s/meta/repair-store",s->storage_paths[0]);
    return n<0 || n>=EFS_MAX_PATH+80?EFS_ERR_INVAL:EFS_OK;
}
int server_repair_store_identity(struct efsd_server *s,uint8_t uuid[16])
{
    if(!uuid)return EFS_ERR_INVAL;
    memset(uuid,0,16);char path[EFS_MAX_PATH+80];int rc=repair_store_path(s,path);if(rc)return rc;
    /* The configured root may be an operator symlink, but meta never may. */
    int root=open(s->storage_paths[0],O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(root<0)return EFS_ERR_IO;
    int meta=openat(root,"meta",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    int missing=meta<0 && errno==ENOENT;
    if(close(root)){if(meta>=0)close(meta);return EFS_ERR_IO;}
    if(meta<0)return missing?EFS_ERR_STALE:EFS_ERR_IO;
    int fd=openat(meta,"repair-store",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    missing=fd<0 && errno==ENOENT;
    if(close(meta)){if(fd>=0)close(fd);return EFS_ERR_IO;}
    if(fd<0)return missing?EFS_ERR_STALE:EFS_ERR_IO;
    uint8_t b[52],sum[32];struct stat st;size_t off=0;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size!=sizeof(b))rc=EFS_ERR_IO;
    while(!rc && off<sizeof(b)){ssize_t n=pread(fd,b+off,sizeof(b)-off,(off_t)off);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){rc=EFS_ERR_IO;break;}off+=(size_t)n;}
    if(close(fd))rc=EFS_ERR_IO;
    if(rc)return rc;
    efs_hash(b,20,sum);uint8_t any=0;for(unsigned i=0;i<16;i++)any|=b[4+i];
    if(memcmp(b,"ERS1",4) || !any || memcmp(sum,b+20,32))return EFS_ERR_IO;
    memcpy(uuid,b+4,16);return EFS_OK;
}
static int repair_store_seal_file(struct efsd_server *s,const char *name)
{
    char path[EFS_MAX_PATH+80];int nn=snprintf(path,sizeof(path),"%s",name);
    if(nn<0 || (size_t)nn>=sizeof(path))return EFS_ERR_INVAL;
    int rc=EFS_OK;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return EFS_ERR_IO;
    int sr;do{sr=fsync(fd);}while(sr<0 && errno==EINTR);
    int closed=close(fd);if(sr<0 || closed)return EFS_ERR_IO;
    /* Seal meta, configured root and its ancestry; an empty new root itself
     * may have been newly named. Existing configured-root symlinks are allowed. */
    int level=0;
    for(;;){char *slash=strrchr(path,'/');if(!slash)return EFS_ERR_IO;
        if(slash==path){rc=put_session_sync_dir("/",1);break;}
        *slash=0;rc=put_session_sync_dir(path,level==1);if(rc)return rc;level++;
    }
    if(rc)return rc;
    /* Also seal the actual root's naming ancestry when configuration uses an
     * alias: lexical ancestor sync alone cannot persist the target name. */
    char *resolved=realpath(s->storage_paths[0],NULL);
    if(!resolved)return EFS_ERR_IO;
    for(;;){rc=put_session_sync_dir(resolved,1);if(rc)break;
        char *slash=strrchr(resolved,'/');if(!slash){rc=EFS_ERR_IO;break;}
        if(slash==resolved){rc=put_session_sync_dir("/",1);break;}*slash=0;
    }
    free(resolved);return rc;
}
static int repair_store_seal_identity(struct efsd_server *s)
{
    char path[EFS_MAX_PATH+80];int rc=repair_store_path(s,path);
    return rc?rc:repair_store_seal_file(s,path);
}
/* Separate durable activation latch survives identity deletion/corruption.
 * Creation is trusted local setup only, never an RPC. No deactivation exists. */
int server_repair_activation(struct efsd_server *s,int create)
{
    if(!s || s->storage_path_count!=1 || (create!=0 && create!=1))return EFS_ERR_INVAL;
    char path[EFS_MAX_PATH+80],dir[EFS_MAX_PATH+80];
    int nn=snprintf(path,sizeof(path),"%s/meta/repair-active",s->storage_paths[0]);
    if(nn<0 || (size_t)nn>=sizeof(path))return EFS_ERR_INVAL;
    strcpy(dir,path);*strrchr(dir,'/')=0;
    pthread_once(&put_session_once,put_session_init);pthread_mutex_t *mu=&put_session_mu[0];pthread_mutex_lock(mu);
    int parent=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW),rc=EFS_OK,fd=-1;
    if(parent<0){pthread_mutex_unlock(mu);return EFS_ERR_IO;}
    uint8_t expected[36]={0},got[36];memcpy(expected,"ERA1",4);efs_hash(expected,4,expected+4);
    fd=openat(parent,"repair-active",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0){
        struct stat st;size_t off=0;
        if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size!=sizeof(got))rc=EFS_ERR_IO;
        while(!rc && off<sizeof(got)){ssize_t n=read(fd,got+off,sizeof(got)-off);
            if(n<0 && errno==EINTR)continue;
            if(n<=0){rc=EFS_ERR_IO;break;}off+=(size_t)n;}
        if(close(fd))rc=EFS_ERR_IO;
        fd=-1;
        if(!rc && memcmp(expected,got,sizeof(got)))rc=EFS_ERR_IO;
    }else if(errno!=ENOENT)rc=EFS_ERR_IO;
    else if(!create)rc=EFS_ERR_STALE;
    else {
#ifdef O_TMPFILE
        fd=openat(parent,".",O_TMPFILE|O_RDWR|O_CLOEXEC,0600);
#else
        rc=EFS_ERR_INVAL;
#endif
        if(fd<0 && !rc)rc=EFS_ERR_IO;
        size_t off=0;while(!rc && off<sizeof(expected)){ssize_t n=write(fd,expected+off,sizeof(expected)-off);
            if(n<0 && errno==EINTR)continue;
            if(n<=0){rc=EFS_ERR_IO;break;}off+=(size_t)n;}
        if(!rc){int sr;do{sr=fsync(fd);}while(sr<0 && errno==EINTR);if(sr<0)rc=EFS_ERR_IO;}
        if(!rc){char proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);
            if(linkat(AT_FDCWD,proc,parent,"repair-active",AT_SYMLINK_FOLLOW))rc=errno==EEXIST?EFS_ERR_BUSY:EFS_ERR_IO;}
        if(fd>=0 && close(fd))rc=EFS_ERR_IO;
    }
    if(close(parent))rc=EFS_ERR_IO;
    /* A prior failed setup may leave a linked but unacknowledged marker.
     * Every successful startup/read repeats file and ancestry barriers. */
    if(!rc)rc=repair_store_seal_file(s,path);
    pthread_mutex_unlock(mu);return rc;
}

int server_repair_store_initialize(struct efsd_server *s,int trusted_existing,uint8_t uuid[16])
{
    if(!uuid || (trusted_existing!=0 && trusted_existing!=1))return EFS_ERR_INVAL;
    pthread_once(&put_session_once,put_session_init);pthread_mutex_t *mu=&put_session_mu[0];
    pthread_mutex_lock(mu);int rc=server_repair_store_identity(s,uuid);
    if(rc==EFS_OK){
        /* A prior link can survive failed directory sync: repeat all barriers. */
        rc=repair_store_seal_identity(s);if(rc)memset(uuid,0,16);
        pthread_mutex_unlock(mu);return rc;
    }
    if(rc!=EFS_ERR_STALE){pthread_mutex_unlock(mu);return rc;}
    if(!trusted_existing){
        DIR *d=opendir(s->storage_paths[0]);if(!d){pthread_mutex_unlock(mu);return EFS_ERR_IO;}
        struct dirent *e;int nonempty=0;errno=0;
        while((e=readdir(d)))if(strcmp(e->d_name,".") && strcmp(e->d_name,"..")){nonempty=1;break;}
        int err=errno;if(closedir(d))err=EIO;
        if(nonempty || err){pthread_mutex_unlock(mu);return err?EFS_ERR_IO:EFS_ERR_STALE;}
    }
    char path[EFS_MAX_PATH+80],dir[EFS_MAX_PATH+80];rc=repair_store_path(s,path);
    if(rc){pthread_mutex_unlock(mu);return rc;}strcpy(dir,path);*strrchr(dir,'/')=0;
    if(mkdir(dir,0755) && errno!=EEXIST){pthread_mutex_unlock(mu);return EFS_ERR_IO;}
    if(put_session_sync_dir(dir,0)){pthread_mutex_unlock(mu);return EFS_ERR_IO;}
    uint8_t b[52]={0};memcpy(b,"ERS1",4);
    int random=open("/dev/urandom",O_RDONLY|O_CLOEXEC);size_t off=0;
    if(random<0)rc=EFS_ERR_IO;
    while(!rc && off<16){ssize_t n=read(random,b+4+off,16-off);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){rc=EFS_ERR_IO;break;}off+=(size_t)n;}
    if(random>=0 && close(random))rc=EFS_ERR_IO;
    efs_hash(b,20,b+20);
#ifdef O_TMPFILE
    int fd=rc==EFS_OK?open(dir,O_TMPFILE|O_RDWR|O_CLOEXEC,0600):-1;
#else
    int fd=-1;rc=EFS_ERR_INVAL;
#endif
    if(fd<0 && !rc)rc=EFS_ERR_IO;
    off=0;while(!rc && off<sizeof(b)){ssize_t n=write(fd,b+off,sizeof(b)-off);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){rc=EFS_ERR_IO;break;}off+=(size_t)n;}
    if(!rc){int sr;do{sr=fsync(fd);}while(sr<0 && errno==EINTR);if(sr<0)rc=EFS_ERR_IO;}
    if(!rc){char proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);
        if(linkat(AT_FDCWD,proc,AT_FDCWD,path,AT_SYMLINK_FOLLOW))rc=errno==EEXIST?EFS_ERR_BUSY:EFS_ERR_IO;}
    if(fd>=0 && close(fd))rc=EFS_ERR_IO;
    if(!rc)rc=repair_store_seal_identity(s);
    if(!rc)rc=server_repair_store_identity(s,uuid);
    if(rc)memset(uuid,0,16);
    pthread_mutex_unlock(mu);return rc;
}
int server_repair_store_matches(struct efsd_server *s,const uint8_t expected[16])
{
    if(!expected)return EFS_ERR_INVAL;
    uint8_t any=0,current[16];for(unsigned i=0;i<16;i++)any|=expected[i];
    if(!any)return EFS_ERR_INVAL;
    int rc=server_repair_store_identity(s,current);
    return rc?rc:memcmp(current,expected,16)?EFS_ERR_STALE:EFS_OK;
}
