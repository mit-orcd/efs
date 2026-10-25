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
/* Certificates this process has written or found and then synced with
 * their parent directories, per stripe, under its mutex. A repeated seal
 * at or below such a floor is already durable: every FENCE_LOC of a revoke
 * seals the same floor once per touched shard, and three directory fsyncs
 * each, in the serial apply, made a 2,058-shard revoke take 24 s (A11
 * finding 1). Process memory only: after a restart the first seal syncs. */
#define PUT_SESSION_SEALED 4u
static struct { uint64_t path; uint8_t uuid[16]; uint32_t floor; }
    put_session_sealed[PUT_SESSION_STRIPES][PUT_SESSION_SEALED];
static unsigned put_session_sealed_next[PUT_SESSION_STRIPES];
static uint64_t sealed_path_hash(const char *path)
{
    uint64_t h=14695981039346656037ull;
    for(const unsigned char *p=(const unsigned char *)path;*p;p++)h=(h^*p)*1099511628211ull;
    return h;
}
static int sealed_durable(unsigned stripe,const char *path,const uint8_t uuid[16],uint32_t floor)
{
    uint64_t h=sealed_path_hash(path);
    for(unsigned i=0;i<PUT_SESSION_SEALED;i++)
        if(put_session_sealed[stripe][i].floor>=floor && put_session_sealed[stripe][i].path==h &&
           !memcmp(put_session_sealed[stripe][i].uuid,uuid,16))return 1;
    return 0;
}
static void sealed_note(unsigned stripe,const char *path,const uint8_t uuid[16],uint32_t floor)
{
    uint64_t h=sealed_path_hash(path);
    for(unsigned i=0;i<PUT_SESSION_SEALED;i++)
        if(put_session_sealed[stripe][i].path==h && !memcmp(put_session_sealed[stripe][i].uuid,uuid,16)) {
            if(put_session_sealed[stripe][i].floor<floor)put_session_sealed[stripe][i].floor=floor;
            return;
        }
    unsigned i=put_session_sealed_next[stripe]++%PUT_SESSION_SEALED;
    put_session_sealed[stripe][i].path=h;
    memcpy(put_session_sealed[stripe][i].uuid,uuid,16);put_session_sealed[stripe][i].floor=floor;
}
static pthread_once_t put_session_once=PTHREAD_ONCE_INIT;
static void put_session_init(void)
{
    for(unsigned i=0;i<PUT_SESSION_STRIPES;i++)pthread_mutex_init(&put_session_mu[i],NULL);
}
static int storage_fence_path(struct efsd_server *s,const uint8_t uuid[16],
                             char path[EFS_MAX_PATH+80],pthread_mutex_t **mu)
{
    if(!s || !uuid || !s->storage_path_count || !s->storage_paths[0][0])return EFS_ERR_INVAL;
    unsigned nonzero=0,hash=0;char hex[33];
    for(unsigned i=0;i<16;i++){nonzero|=uuid[i];hash=hash*33u+uuid[i];snprintf(hex+2*i,3,"%02x",uuid[i]);}
    if(!nonzero)return EFS_ERR_INVAL;
    int n=snprintf(path,EFS_MAX_PATH+80,"%s/meta/put-session/%s",s->storage_paths[0],hex);
    if(n<0 || n>=EFS_MAX_PATH+80)return EFS_ERR_INVAL;
    pthread_once(&put_session_once,put_session_init);
    *mu=&put_session_mu[hash%PUT_SESSION_STRIPES];return EFS_OK;
}
static int storage_fence_read(const char *path,const uint8_t uuid[16],uint32_t *floor)
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
    if(memcmp(b,"EPF1",4) || memcmp(b+4,uuid,16) || memcmp(b+24,sum,32))return EFS_ERR_IO;
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
static int storage_fence_begin(struct efsd_server *s,const uint8_t uuid[16],
                             uint32_t epoch,struct efs_put_storage_guard *guard)
{
    if(!epoch || !guard || guard->locked)return EFS_ERR_INVAL;
    char path[EFS_MAX_PATH+80];pthread_mutex_t *mu;
    int rc=storage_fence_path(s,uuid,path,&mu);if(rc!=EFS_OK)return rc;
    pthread_mutex_lock(mu);uint32_t floor=0;rc=storage_fence_read(path,uuid,&floor);
    if(rc==EFS_OK && epoch<floor)rc=EFS_ERR_STALE;
    if(rc==EFS_OK)guard->locked=mu;else pthread_mutex_unlock(mu);
    return rc;
}
void server_put_session_end(struct efs_put_storage_guard *guard)
{
    if(guard && guard->locked){pthread_mutex_t *mu=guard->locked;guard->locked=NULL;pthread_mutex_unlock(mu);}
}
static int storage_fence_seal(struct efsd_server *s,const uint8_t uuid[16],uint32_t floor)
{
    if(!floor)return EFS_ERR_INVAL;
    char path[EFS_MAX_PATH+80];pthread_mutex_t *mu;
    int rc=storage_fence_path(s,uuid,path,&mu);if(rc!=EFS_OK)return rc;
    unsigned stripe=(unsigned)(mu-put_session_mu);
    pthread_mutex_lock(mu);uint32_t old=0;rc=storage_fence_read(path,uuid,&old);
    if(rc==EFS_OK && floor<=old && sealed_durable(stripe,path,uuid,floor)) {
        pthread_mutex_unlock(mu);return EFS_OK;
    }
    if(rc==EFS_OK && floor>old){
        char dir[EFS_MAX_PATH+80];strcpy(dir,path);*strrchr(dir,'/')=0;
        char *slash=strrchr(dir,'/');*slash=0;
        if(mkdir(dir,0755) && errno!=EEXIST)rc=EFS_ERR_IO;
        *slash='/';if(rc==EFS_OK && mkdir(dir,0755) && errno!=EEXIST)rc=EFS_ERR_IO;
        /* Refuse symlink directories before creating/replacing a certificate. */
        if(rc==EFS_OK)rc=put_session_sync_dir(dir,0);
        uint8_t b[PUT_SESSION_CERT_BYTES]={0};memcpy(b,"EPF1",4);memcpy(b+4,uuid,16);
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
    /* The file was fsynced before its rename, or read back from disk; with
     * its parents synced, the larger floor is durable. */
    if(rc==EFS_OK)sealed_note(stripe,path,uuid,floor>old?floor:old);
    pthread_mutex_unlock(mu);return rc;
}

int server_put_session_begin(struct efsd_server *s,const uint8_t uuid[16],uint32_t epoch,
                             struct efs_put_storage_guard *guard)
{return storage_fence_begin(s,uuid,epoch,guard);}
int server_put_session_fence(struct efsd_server *s,const uint8_t uuid[16],uint32_t floor)
{return storage_fence_seal(s,uuid,floor);}
int server_put_session_floor(struct efsd_server *s,const uint8_t uuid[16],uint32_t *floor)
{
    char path[EFS_MAX_PATH+80];pthread_mutex_t *mu;
    if(!floor)return EFS_ERR_INVAL;
    *floor=0;int rc=storage_fence_path(s,uuid,path,&mu);if(rc!=EFS_OK)return rc;
    pthread_mutex_lock(mu);rc=storage_fence_read(path,uuid,floor);pthread_mutex_unlock(mu);
    return rc;
}
