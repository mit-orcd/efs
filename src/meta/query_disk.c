/* Local, rebuildable query cache. Authoritative metadata remains in Raft/KV.
 * Immutable generation directories retain old query leases. Only changed raw
 * partitions are copied; derived records and the rollup queue live on disk.
 * No allocation is proportional to namespace cardinality. Native record ABI
 * is versioned in the manifest; restoration requires matching authoritative
 * watermarks and verified content digests.
 */
#include "query_disk.h"
#include "blake3.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#define SHARDS 4096
#define KINDS 6
#define FD_CACHE 32
#define READ_WINDOW 8192
#define READ_WAYS 4
_Static_assert(FD_CACHE<=UINT8_MAX,"descriptor index must fit");
#define SORT_ROWS 2048
#define NODE_BATCH 8
#define VALUE_MAX 16384
#define RAW_RECORD_MAX (EFS_KV_KEY_MAX+VALUE_MAX)
static const uint8_t kinds[KINDS]={EFS_KV_KIND_INODE,EFS_KV_KIND_DENTRY,
    EFS_KV_KIND_LANE,EFS_KV_KIND_DSEQ,EFS_KV_KIND_APPEND_CUR,EFS_KV_KIND_PVER};
struct slot { uint64_t offset;uint32_t key_len,value_len; };
struct disk_part { uint64_t revision,count,inodes,names,begin[KINDS],end[KINDS];uint8_t digest[4][32]; };
struct disk_node {
    struct efs_meta_row row;
    struct efs_meta_stat stat;
    struct efs_query_rollup rollup;
    struct efs_name_entry link;
    uint64_t children;
};
/* One candidate-only accumulator; never retained across generations. */
struct fold_parent { struct disk_node node;uint64_t at;int loaded; };
struct read_window {
    uint64_t offset,tick;size_t size;
    uint8_t data[READ_WINDOW];
};
struct fd_slot {
    int fd;uint32_t shard;char kind;uint64_t tick,window_tick;
    struct read_window windows[READ_WAYS];
};
struct efs_query_disk {
    char path[EFS_MAX_PATH];
    struct disk_part part[SHARDS];
    struct fd_slot fds[FD_CACHE];
    uint8_t shared_d[SHARDS]; /* Immutable old generation link until first write. */
    uint8_t fd_index[SHARDS][4]; /* Slot plus one; zero means absent. */
    pthread_mutex_t mutex;
    uint64_t tick,count,bytes,limit,disk_bytes,disk_limit,root_bytes,root_limit;
    int budget_fd;
    uint32_t rebuilt,refolded;
    uint64_t watermark[4];
    struct efs_kv kv;
    int (*stop)(void *);void *stop_ctx;
};
static int cancelled(const struct efs_query_disk *d) { return d->stop && d->stop(d->stop_ctx); }
static int kind_at(uint8_t kind) { for(int i=0;i<KINDS;i++)if(kinds[i]==kind)return i;return -1; }
static pthread_once_t reserve_once=PTHREAD_ONCE_INIT;
static uint64_t reserve_bytes=UINT64_C(64)<<20;
static uint64_t generation_limit=UINT64_C(16384)<<20;
static uint64_t aggregate_limit=UINT64_C(32768)<<20;
static void configure_reserve(void)
{
    const char *text=getenv("EFS_QUERY_DISK_RESERVE_MIB");
    char *end=NULL;errno=0;unsigned long long mib=text?strtoull(text,&end,10):64;
    if(text && (errno || !end || *end || mib<1 || mib>1048576))
        fprintf(stderr,"query-config: invalid EFS_QUERY_DISK_RESERVE_MIB; using 64\n");
    else reserve_bytes=(uint64_t)mib<<20;
    text=getenv("EFS_QUERY_DISK_MAX_MIB");
    errno=0;end=NULL;mib=text?strtoull(text,&end,10):16384;
    if(text && (errno || !end || *end || mib<1 || mib>16777216))
        fprintf(stderr,"query-config: invalid EFS_QUERY_DISK_MAX_MIB; using 16384\n");
    else generation_limit=(uint64_t)mib<<20;
    text=getenv("EFS_QUERY_DISK_TOTAL_MIB");
    errno=0;end=NULL;mib=text?strtoull(text,&end,10):32768;
    if(text && (errno || !end || *end || mib<1 || mib>16777216))
        fprintf(stderr,"query-config: invalid EFS_QUERY_DISK_TOTAL_MIB; using 32768\n");
    else aggregate_limit=(uint64_t)mib<<20;
}
/* A free-space guard, not a reservation against concurrent non-cache writers.
 * Check every 256 small writes; ENOSPC still propagates from every syscall. */
static int disk_reserve(int fd)
{
    static __thread unsigned writes;
    if((writes++ & 255u)!=0)return EFS_OK;
    pthread_once(&reserve_once,configure_reserve);
    struct statvfs fs;
    if(fstatvfs(fd,&fs))return EFS_ERR_IO;
    uint64_t block=fs.f_frsize;
    if(block && fs.f_bavail<reserve_bytes/block+(reserve_bytes%block!=0))return EFS_ERR_IO;
    return EFS_OK;
}
/* Conservative logical storage admission: raw/index bytes, two name-sort
 * copies, derived inode rows and a queue entry per inode. Shared hard links
 * are charged to each generation. Filesystem allocation overhead and other
 * retained generations remain separate from this per-generation limit. */
static int charge_disk(struct efs_query_disk *d,uint64_t bytes)
{
    if(bytes>d->disk_limit || d->disk_bytes>d->disk_limit-bytes)return EFS_ERR_QUOTA;
    if(d->root_limit && (d->root_bytes>d->root_limit ||
       d->disk_bytes>d->root_limit-d->root_bytes ||
       bytes>d->root_limit-d->root_bytes-d->disk_bytes))return EFS_ERR_QUOTA;
    d->disk_bytes+=bytes;return EFS_OK;
}
#include "query_disk_budget.h"
static uint64_t derived_charge(uint64_t inodes,uint64_t names)
{
    return inodes*(sizeof(struct disk_node)+sizeof(efs_ino_t))+
           names*(2*sizeof(struct efs_name_entry));
}
static int io_at(int fd,void *buf,size_t n,uint64_t off,int write)
{
    if(off>INT64_MAX || n>INT64_MAX-off)return EFS_ERR_OVERFLOW;
    if(write && disk_reserve(fd))return EFS_ERR_IO;
    uint8_t *p=buf;
    while(n) {
        ssize_t got=write ? pwrite(fd,p,n,(off_t)off) : pread(fd,p,n,(off_t)off);
        if(got<0 && errno==EINTR)continue;
        if(got<=0)return EFS_ERR_IO;
        p+=got;n-=(size_t)got;off+=(uint64_t)got;
    }
    return EFS_OK;
}
static int filename(const struct efs_query_disk *d,uint32_t shard,char kind,char *p,size_t cap)
{
    int n=snprintf(p,cap,"%s/%04x.%c",d->path,shard,kind);
    return n<0 || (size_t)n>=cap ? EFS_ERR_NAMETOOLONG : EFS_OK;
}
/* Called under the candidate mutex before any derived write. Never modify a
 * hard-linked old generation. Reflink is optional; the checked copy fallback
 * works on non-reflink filesystems too. The publication barrier syncs the copy. */
static int private_derived(struct efs_query_disk *d,uint32_t shard)
{
    if(!d->shared_d[shard])return EFS_OK;
    char path[EFS_MAX_PATH],tmp[EFS_MAX_PATH];
    int rc=filename(d,shard,'d',path,sizeof(path));if(rc)return rc;
    int len=snprintf(tmp,sizeof(tmp),"%s.copy",path);
    if(len<0 || (size_t)len>=sizeof(tmp))return EFS_ERR_NAMETOOLONG;
    int src=open(path,O_RDONLY|O_CLOEXEC),dst=open(tmp,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
    if(src<0 || dst<0)rc=EFS_ERR_IO;
    int cloned=0;uint64_t temporary=0;
    if(!rc){
        struct stat st;
        if(fstat(src,&st) || st.st_size<0)rc=EFS_ERR_IO;
        else{rc=charge_disk(d,(uint64_t)st.st_size);if(!rc)temporary=(uint64_t)st.st_size;}
    }
    if(!rc && disk_reserve(dst))rc=EFS_ERR_IO;
#ifdef __linux__
    if(!rc){
        cloned=ioctl(dst,FICLONE,src)==0;
        if(!cloned && errno!=EOPNOTSUPP && errno!=ENOTTY && errno!=EXDEV && errno!=EINVAL)rc=EFS_ERR_IO;
    }
#endif
    if(!rc && !cloned){
        uint8_t buf[65536];uint64_t off=0;
        while(!rc){
            if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
            ssize_t n=pread(src,buf,sizeof(buf),(off_t)off);
            if(n<0 && errno==EINTR)continue;
            if(n<0){rc=EFS_ERR_IO;break;}
            if(!n)break;
            rc=io_at(dst,buf,(size_t)n,off,1);off+=(uint64_t)n;
        }
    }
    if(src>=0)close(src);
    if(dst>=0)close(dst);
    if(!rc && rename(tmp,path))rc=EFS_ERR_IO;
    d->disk_bytes-=temporary;
    if(rc){unlink(tmp);return rc;}
    /* Any cached descriptor/windows refer to the old immutable inode. */
    uint8_t slot=d->fd_index[shard][3];
    if(slot){close(d->fds[slot-1].fd);d->fds[slot-1].fd=-1;d->fd_index[shard][3]=0;}
    d->shared_d[shard]=0;return EFS_OK;
}
/* The descriptor cannot be evicted while pread/pwrite is using it. Reads use
 * independent offsets, and this lock never spans callbacks or recursive KV. */
/* Four fixed LRU windows per descriptor amortize binary-search and adjacent-record syscalls.
 * Files are immutable except for this candidate's derived rows. Writes remain
 * synchronous to the descriptor; no dirty data is retained only in this cache. */
static int window_read(struct fd_slot *f,void *buf,size_t n,uint64_t off)
{
    uint64_t base=off & ~(uint64_t)(READ_WINDOW-1);
    if(n>READ_WINDOW-(off-base))return io_at(f->fd,buf,n,off,0);
    struct read_window *w=NULL,*victim=&f->windows[0];
    for(unsigned i=0;i<READ_WAYS;i++){
        struct read_window *candidate=&f->windows[i];
        if(candidate->size && candidate->offset==base){w=candidate;break;}
        if(!candidate->size || (victim->size && candidate->tick<victim->tick))victim=candidate;
    }
    size_t at=(size_t)(off-base);
    /* A later write may extend a formerly short EOF block, even through
     * a sparse gap. Refill when the requested bytes exceed that old tail. */
    if(w && (at>w->size || n>w->size-at))w->size=0;
    if(!w || !w->size){
        if(!w)w=victim;
        w->offset=base;w->size=0;
        size_t cap=READ_WINDOW;
        if((uint64_t)cap>INT64_MAX-base)cap=(size_t)(INT64_MAX-base);
        while(w->size<cap){
            ssize_t got=pread(f->fd,w->data+w->size,cap-w->size,(off_t)(base+w->size));
            if(got<0 && errno==EINTR)continue;
            if(got<0){w->size=0;return EFS_ERR_IO;}
            if(!got)break;
            w->size+=(size_t)got;
        }
    }
    w->tick=++f->window_tick;
    if(at>w->size || n>w->size-at)return EFS_ERR_IO;
    memcpy(buf,w->data+at,n);return EFS_OK;
}
static int fd_kind(char kind)
{
    switch(kind){case 'r':return 0;case 'i':return 1;case 'n':return 2;case 'd':return 3;default:return -1;}
}
static int disk_io(struct efs_query_disk *d,uint32_t shard,char kind,void *buf,size_t n,uint64_t off,int write)
{
    int k=fd_kind(kind);if(shard>=SHARDS || k<0)return EFS_ERR_INVAL;
    if(off>INT64_MAX || n>INT64_MAX-off)return EFS_ERR_OVERFLOW;
    if(!n)return EFS_OK;
    int rc=EFS_OK;
    pthread_mutex_lock(&d->mutex);
    if(write && kind=='d'){rc=private_derived(d,shard);if(rc){pthread_mutex_unlock(&d->mutex);return rc;}}
    uint8_t found=d->fd_index[shard][k];
    struct fd_slot *f=found?&d->fds[found-1]:NULL,*victim=&d->fds[0];
    if(!f)for(int i=0;i<FD_CACHE;i++) {
        if(d->fds[i].fd<0){victim=&d->fds[i];break;}
        if(d->fds[i].tick<victim->tick)victim=&d->fds[i];
    }
    if(!f) {
        char path[EFS_MAX_PATH];rc=filename(d,shard,kind,path,sizeof(path));
        if(!rc) {
            f=victim;
            if(f->fd>=0){d->fd_index[f->shard][fd_kind(f->kind)]=0;close(f->fd);}
            f->window_tick=0;
            for(unsigned i=0;i<READ_WAYS;i++)f->windows[i].size=0;
            f->fd=open(path,O_RDWR|O_CLOEXEC);f->shard=shard;f->kind=kind;
            if(f->fd<0)rc=EFS_ERR_IO;
            else d->fd_index[shard][k]=(uint8_t)(f-d->fds+1);
        }
    }
    if(!rc) {
        f->tick=++d->tick;
        if(!write)rc=window_read(f,buf,n,off);
        else {
            rc=io_at(f->fd,buf,n,off,1);
            for(unsigned i=0;i<READ_WAYS;i++){
                struct read_window *w=&f->windows[i];
                if(rc)w->size=0; /* A failed write may have changed a prefix. */
                else if(w->size && off<w->offset+w->size && off+n>w->offset){
                    uint64_t lo=off>w->offset?off:w->offset;
                    uint64_t hi=off+n<w->offset+w->size?off+n:w->offset+w->size;
                    memcpy(w->data+(size_t)(lo-w->offset),(uint8_t *)buf+(size_t)(lo-off),(size_t)(hi-lo));
                }
            }
        }
    }
    pthread_mutex_unlock(&d->mutex);return rc;
}
static void close_cache(struct efs_query_disk *d)
{
    for(int i=0;i<FD_CACHE;i++)if(d->fds[i].fd>=0){close(d->fds[i].fd);d->fds[i].fd=-1;}
    memset(d->fd_index,0,sizeof(d->fd_index));
}
void efs_query_disk_free(struct efs_query_disk *d)
{
    if(!d)return;
    close_cache(d);
    char current[EFS_MAX_PATH],target[EFS_MAX_PATH];
    const char *base=strrchr(d->path,'/');int keep=0;
    if(base) {
        int n=snprintf(current,sizeof(current),"%.*s/current",(int)(base-d->path),d->path);
        ssize_t len=n>0 && (size_t)n<sizeof(current)?readlink(current,target,sizeof(target)-1):-1;
        if(len>0){target[len]=0;keep=!strcmp(target,base+1);}
    }
    DIR *dir=keep?NULL:opendir(d->path);
    if(dir) {
        struct dirent *e;
        while((e=readdir(dir))) {
            if(!strcmp(e->d_name,".") || !strcmp(e->d_name,".."))continue;
            char path[EFS_MAX_PATH];int n=snprintf(path,sizeof(path),"%s/%s",d->path,e->d_name);
            if(n>0 && (size_t)n<sizeof(path))unlink(path);
        }
        closedir(dir);rmdir(d->path);
    }
    if(d->budget_fd>=0)close(d->budget_fd);
    pthread_mutex_destroy(&d->mutex);free(d);
}
/* Acquire once per host worker, before opening any generation. The lease
 * prevents recovery from deleting snapshots belonging to another publisher.
 * Never traverse symlinks or recursively delete unknown directories. */
int efs_query_catalog_disk_recover(const char *root,int *lease)
{
    if(!root || !lease)return EFS_ERR_INVAL;
    *lease=-1;
    if(mkdir(root,0700) && errno!=EEXIST)return EFS_ERR_IO;
    int dirfd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dirfd<0)return EFS_ERR_IO;
    int lock=openat(dirfd,"publisher.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lock<0){close(dirfd);return EFS_ERR_IO;}
    if(flock(lock,LOCK_EX|LOCK_NB)){close(lock);close(dirfd);return EFS_ERR_BUSY;}
    char current[256]={0};ssize_t n=readlinkat(dirfd,"current",current,sizeof(current)-1);
    if(n>=0)current[n]=0;
    else if(errno!=ENOENT){close(lock);close(dirfd);return EFS_ERR_IO;}
    if(n>=0 && (strncmp(current,"view-",5) || strchr(current,'/'))){close(lock);close(dirfd);return EFS_ERR_PROTO;}
    DIR *dir=fdopendir(dup(dirfd));int rc=dir?EFS_OK:EFS_ERR_IO;
    struct dirent *entry;
    while(dir && (entry=readdir(dir))) {
        if(strncmp(entry->d_name,"view-",5) || !strcmp(entry->d_name,current))continue;
        int child=openat(dirfd,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(child<0){rc=EFS_ERR_IO;break;}
        DIR *files=fdopendir(dup(child));
        if(!files){close(child);rc=EFS_ERR_IO;break;}
        struct dirent *file;
        while((file=readdir(files))) {
            if(!strcmp(file->d_name,".") || !strcmp(file->d_name,".."))continue;
            struct stat st;
            if(fstatat(child,file->d_name,&st,AT_SYMLINK_NOFOLLOW) || S_ISDIR(st.st_mode) || unlinkat(child,file->d_name,0)){rc=EFS_ERR_IO;break;}
        }
        closedir(files);close(child);
        if(rc || unlinkat(dirfd,entry->d_name,AT_REMOVEDIR)){rc=EFS_ERR_IO;break;}
    }
    if(dir)closedir(dir);
    if(!rc && unlinkat(dirfd,"current.new",0) && errno!=ENOENT)rc=EFS_ERR_IO;
    if(!rc && fsync(dirfd))rc=EFS_ERR_IO;
    close(dirfd);
    if(rc)close(lock);else *lease=lock;
    return rc;
}
static int cmpkey(const uint8_t *a,uint32_t an,const uint8_t *b,uint32_t bn)
{
    uint32_t n=an<bn?an:bn;int rc=memcmp(a,b,n);
    return rc ? rc : an<bn ? -1 : an>bn ? 1 : 0;
}
static int read_record(struct efs_query_disk *d,uint32_t shard,uint64_t at,struct slot *s,uint8_t *key,uint8_t *value)
{
    if(at>=d->part[shard].count)return EFS_ERR_NOT_FOUND;
    int rc=disk_io(d,shard,'i',s,sizeof(*s),at*sizeof(*s),0);
    if(rc)return rc;
    if(s->key_len<3 || s->key_len>EFS_KV_KEY_MAX || s->value_len>VALUE_MAX ||
       s->offset>(uint64_t)INT64_MAX-s->key_len-s->value_len)return EFS_ERR_PROTO;
    rc=disk_io(d,shard,'r',key,s->key_len,s->offset,0);
    if(!rc && value)rc=disk_io(d,shard,'r',value,s->value_len,s->offset+s->key_len,0);
    return rc;
}
static int seek(struct efs_query_disk *d,const uint8_t *key,uint32_t kl,uint64_t *at,struct slot *exact)
{
    if(exact)memset(exact,0,sizeof(*exact));
    if(kl<3 || kl>EFS_KV_KEY_MAX)return EFS_ERR_INVAL;
    uint32_t shard=((uint32_t)key[0]<<8)|key[1];int kind=kind_at(key[2]);
    if(shard>=SHARDS || kind<0)return EFS_ERR_NOT_FOUND;
    uint64_t lo=d->part[shard].begin[kind],hi=d->part[shard].end[kind];
    while(lo<hi) {
        uint64_t mid=lo+(hi-lo)/2;struct slot s;uint8_t k[EFS_KV_KEY_MAX];
        int rc=read_record(d,shard,mid,&s,k,NULL);if(rc)return rc;
        int order=cmpkey(k,s.key_len,key,kl);
        /* Snapshot keys are unique. Equality is also the lower bound, so
         * return the already validated slot instead of reading it again. */
        if(!order){*at=mid;if(exact)*exact=s;return EFS_OK;}
        if(order<0)lo=mid+1;else hi=mid;
    }
    *at=lo;return EFS_OK;
}
static int raw_get(void *ctx,const uint8_t *key,uint32_t kl,uint8_t *value,uint32_t *vl)
{
    struct efs_query_disk *d=ctx;uint64_t at;struct slot s;
    int rc=seek(d,key,kl,&at,&s);if(rc)return rc;
    if(!s.key_len)return EFS_ERR_NOT_FOUND;
    uint32_t shard=((uint32_t)key[0]<<8)|key[1];
    if(*vl<s.value_len || (s.value_len && !value)){*vl=s.value_len;return EFS_ERR_INVAL;}
    rc=disk_io(d,shard,'r',value,s.value_len,s.offset+s.key_len,0);*vl=s.value_len;return rc;
}
static int raw_scan(void *ctx,const uint8_t *prefix,uint32_t pn,const uint8_t *start,uint32_t sn,
    int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *user)
{
    struct efs_query_disk *d=ctx;uint64_t at;
    if(pn<3)return EFS_ERR_INVAL;
    int kind=kind_at(prefix[2]);if(kind<0)return EFS_OK;
    int rc=seek(d,sn?start:prefix,sn?sn:pn,&at,NULL);if(rc==EFS_ERR_NOT_FOUND)return EFS_OK;if(rc)return rc;
    uint32_t shard=((uint32_t)prefix[0]<<8)|prefix[1];
    for(;at<d->part[shard].end[kind];at++) {
        struct slot s;uint8_t key[EFS_KV_KEY_MAX],value[VALUE_MAX];
        rc=read_record(d,shard,at,&s,key,value);if(rc)return rc;
        if(s.key_len<pn || memcmp(key,prefix,pn))break;
        rc=cb(user,key,s.key_len,value,s.value_len);if(rc)return rc;
    }
    return EFS_OK;
}
static int raw_prefix(void *ctx,const uint8_t *p,uint32_t pn,
    int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *u)
{ return raw_scan(ctx,p,pn,NULL,0,cb,u); }
static const struct efs_kv_ops raw_ops={.get=raw_get,.scan_prefix=raw_prefix,.scan_from=raw_scan};
static int make_file(const struct efs_query_disk *,uint32_t,char);
struct writer { struct efs_query_disk *d;uint32_t shard;int raw,index;uint64_t offset,count; };
static int save(void *ctx,const uint8_t *key,uint32_t kl,const uint8_t *val,uint32_t vl)
{
    struct writer *w=ctx;if(cancelled(w->d))return EFS_ERR_BUSY;
    if(kl<3)return EFS_ERR_PROTO;
    if(kl>EFS_KV_KEY_MAX || vl>VALUE_MAX || w->count==UINT32_MAX)return EFS_ERR_OVERFLOW;
    if(w->offset>(uint64_t)INT64_MAX-kl-vl)return EFS_ERR_OVERFLOW;
    uint64_t extra=(uint64_t)kl+vl+sizeof(struct slot)+
        derived_charge(key[2]==EFS_KV_KIND_INODE,key[2]==EFS_KV_KIND_DENTRY);
    int admitted=charge_disk(w->d,extra);if(admitted)return admitted;
    if(w->raw<0)w->raw=make_file(w->d,w->shard,'r');
    if(w->index<0)w->index=make_file(w->d,w->shard,'i');
    if(w->raw<0 || w->index<0)return EFS_ERR_IO;
    struct slot s={w->offset,kl,vl};
    /* Adjacent key/value bytes form one bounded record. Submit them together
     * instead of making a disk syscall for each small field. The index is
     * written only after the complete record succeeds. */
    uint8_t record[RAW_RECORD_MAX];memcpy(record,key,kl);
    if(vl)memcpy(record+kl,val,vl);
    int rc=io_at(w->raw,record,(size_t)kl+vl,w->offset,1);
    if(!rc)rc=io_at(w->index,&s,sizeof(s),w->count*sizeof(s),1);
    if(!rc){w->offset+=kl+vl;w->count++;}return rc;
}
static int pending(void *u,const uint8_t *k,uint32_t kn,const uint8_t *v,uint32_t vn)
{ (void)u;(void)k;(void)kn;(void)v;(void)vn;return EFS_ERR_BUSY; }
static int make_file(const struct efs_query_disk *d,uint32_t shard,char kind)
{
    char p[EFS_MAX_PATH];if(filename(d,shard,kind,p,sizeof(p)))return -1;
    return open(p,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
}
static int raw_build(struct efs_query_disk *d,struct efs_kv_lsm_snapshot *source,uint32_t shard)
{
    uint8_t prefix[3]={(uint8_t)(shard>>8),(uint8_t)shard,0};
    const uint8_t refuse[]={EFS_KV_KIND_INTENT,EFS_KV_KIND_GUARD,EFS_KV_KIND_REDUCE};
    for(unsigned i=0;i<sizeof(refuse);i++){
        prefix[2]=refuse[i];int rc=efs_kv_lsm_snapshot_scan(source,prefix,3,NULL,0,pending,NULL);if(rc)return rc;
    }
    struct writer w={.d=d,.shard=shard,.raw=-1,.index=-1};
    int rc=EFS_OK;
    for(int i=0;i<KINDS && !rc;i++){
        prefix[2]=kinds[i];d->part[shard].begin[i]=w.count;
        rc=efs_kv_lsm_snapshot_scan(source,prefix,3,NULL,0,save,&w);
        d->part[shard].end[i]=w.count;
    }
    d->part[shard].count=w.count;d->part[shard].inodes=d->part[shard].end[0]-d->part[shard].begin[0];
    d->part[shard].names=d->part[shard].end[1]-d->part[shard].begin[1];
    if(!rc && w.count && (fsync(w.raw) || fsync(w.index)))rc=EFS_ERR_IO;
    if(w.raw>=0)close(w.raw);
    if(w.index>=0)close(w.index);
    return rc;
}
static int entry_from(const uint8_t *key,uint32_t kl,const uint8_t *val,uint32_t vl,struct efs_name_entry *e)
{
    if(kl<=11 || kl-11>=EFS_MAX_NAME || memchr(key+11,0,kl-11) || memchr(key+11,'/',kl-11))return EFS_ERR_PROTO;
    memset(e,0,sizeof(*e));int rc=efs_meta_unpack_dentry(val,vl,&e->dentry);if(rc)return rc;
    for(unsigned i=3;i<11;i++)e->parent=(e->parent<<8)|key[i];
    memcpy(e->name,key+11,kl-11);return EFS_OK;
}
static int name_compare(const void *ap,const void *bp)
{
    const struct efs_name_entry *a=ap,*b=bp;int rc=strcmp(a->name,b->name);
    return rc ? rc : a->parent<b->parent ? -1 : a->parent>b->parent ? 1 : 0;
}
static int run_path(struct efs_query_disk *d,uint32_t shard,unsigned pass,uint64_t run,char *p)
{
    int n=snprintf(p,EFS_MAX_PATH,"%s/%04x.tmp%u-%llu",d->path,shard,pass,(unsigned long long)run);
    return n<0 || n>=EFS_MAX_PATH ? EFS_ERR_NAMETOOLONG : EFS_OK;
}
static int names_build(struct efs_query_disk *d,uint32_t shard)
{
    uint64_t count=d->part[shard].names;if(!count)return EFS_OK;
    struct efs_name_entry *rows=malloc(SORT_ROWS*sizeof(*rows));if(!rows)return EFS_ERR_NOMEM;
    uint64_t runs=0;int rc=EFS_OK;char path[EFS_MAX_PATH];
    for(uint64_t at=0;at<count && !rc;at+=SORT_ROWS) {
        unsigned n=(unsigned)(count-at>SORT_ROWS?SORT_ROWS:count-at);
        if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
        for(unsigned i=0;i<n && !rc;i++){
            struct slot s;uint8_t key[EFS_KV_KEY_MAX],val[VALUE_MAX];
            rc=read_record(d,shard,d->part[shard].begin[1]+at+i,&s,key,val);
            if(!rc)rc=entry_from(key,s.key_len,val,s.value_len,&rows[i]);
        }
        if(rc)break;
        qsort(rows,n,sizeof(*rows),name_compare);rc=run_path(d,shard,0,runs++,path);
        if(rc)break;
        int fd=open(path,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
        if(fd<0){rc=EFS_ERR_IO;break;}
        rc=io_at(fd,rows,n*sizeof(*rows),0,1);close(fd);
    }
    free(rows);unsigned pass=0;
    while(runs>1 && !rc) {
        for(uint64_t i=0;i<runs && !rc;i+=2) {
            char a[EFS_MAX_PATH],b[EFS_MAX_PATH],out[EFS_MAX_PATH];
            rc=run_path(d,shard,pass,i,a);
            if(!rc)rc=run_path(d,shard,pass+1,i/2,out);
            if(rc)break;
            if(i+1==runs){if(rename(a,out))rc=EFS_ERR_IO;continue;}
            rc=run_path(d,shard,pass,i+1,b);if(rc)break;
            int fa=open(a,O_RDONLY|O_CLOEXEC),fb=open(b,O_RDONLY|O_CLOEXEC),fo=open(out,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
            struct stat sa,sb;uint64_t ia=0,ib=0,at=0;
            if(fa<0 || fb<0 || fo<0 || fstat(fa,&sa) || fstat(fb,&sb))rc=EFS_ERR_IO;
            uint64_t na=rc?0:(uint64_t)sa.st_size/sizeof(struct efs_name_entry),nb=rc?0:(uint64_t)sb.st_size/sizeof(struct efs_name_entry);
            struct efs_name_entry ea,eb;int ha=0,hb=0;
            while(!rc && (ia<na || ib<nb)) {
                if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
                if(ia<na && !ha){rc=io_at(fa,&ea,sizeof(ea),ia*sizeof(ea),0);ha=1;}
                if(!rc && ib<nb && !hb){rc=io_at(fb,&eb,sizeof(eb),ib*sizeof(eb),0);hb=1;}
                if(rc)break;
                int takea=ia<na && (ib==nb || name_compare(&ea,&eb)<=0);
                rc=io_at(fo,takea?&ea:&eb,sizeof(ea),at++*sizeof(ea),1);
                if(takea){ia++;ha=0;}else{ib++;hb=0;}
            }
            if(fa>=0)close(fa);
            if(fb>=0)close(fb);
            if(fo>=0)close(fo);
            if(!rc){unlink(a);unlink(b);}
        }
        runs=(runs+1)/2;pass++;
    }
    if(!rc) {
        rc=run_path(d,shard,pass,0,path);char final[EFS_MAX_PATH];
        if(!rc)rc=filename(d,shard,'n',final,sizeof(final));
        if(!rc && rename(path,final))rc=EFS_ERR_IO;
    }
    return rc;
}
static int node_at(struct efs_query_disk *d,uint32_t shard,uint64_t at,struct disk_node *n,int write)
{
    if(at>=d->part[shard].inodes)return EFS_ERR_NOT_FOUND;
    if(at>INT64_MAX/sizeof(*n))return EFS_ERR_OVERFLOW;
    return disk_io(d,shard,'d',n,sizeof(*n),at*sizeof(*n),write);
}
static int locate(struct efs_query_disk *d,efs_ino_t ino,struct disk_node *n,uint64_t *at)
{
    uint32_t shard=efs_kv_inode_shard(ino);uint64_t lo=0,hi=d->part[shard].inodes;
    while(lo<hi) {
        uint64_t mid=lo+(hi-lo)/2;int rc=node_at(d,shard,mid,n,0);if(rc)return rc;
        /* Build validation requires strictly increasing inode IDs. An exact
         * match is already the sole answer; do not finish a lower-bound search
         * and read the same row again. */
        if(n->row.ino==ino){if(at)*at=mid;return EFS_OK;}
        if(n->row.ino<ino)lo=mid+1;else hi=mid;
    }
    if(lo==d->part[shard].inodes)return EFS_ERR_NOT_FOUND;
    int rc=node_at(d,shard,lo,n,0);if(rc)return rc;
    if(n->row.ino!=ino)return EFS_ERR_NOT_FOUND;
    if(at)*at=lo;
    return EFS_OK;
}
static int refuse_coord(void *ctx,const struct efs_txid *tx,uint32_t shard,int *decision)
{ (void)ctx;(void)tx;(void)shard;(void)decision;return EFS_ERR_BUSY; }
static int nodes_build(struct efs_query_disk *d,uint32_t shard)
{
    if(!d->part[shard].inodes)return EFS_OK;
    int fd=make_file(d,shard,'d');if(fd<0)return EFS_ERR_IO;
    int rc=EFS_OK;efs_ino_t previous=0;
    struct disk_node batch[NODE_BATCH];unsigned used=0;
    for(uint64_t i=0;i<d->part[shard].inodes && !rc;i++) {
        if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
        struct slot s;uint8_t key[EFS_KV_KEY_MAX],val[VALUE_MAX];struct disk_node n={0};
        rc=read_record(d,shard,d->part[shard].begin[0]+i,&s,key,val);
        if(!rc)rc=efs_meta_unpack_inode(val,s.value_len,&n.row);
        efs_ino_t encoded=0;
        if(!rc && s.key_len!=11)rc=EFS_ERR_PROTO;
        if(!rc)for(unsigned k=3;k<11;k++)encoded=(encoded<<8)|key[k];
        if(!rc && (encoded!=n.row.ino || efs_kv_inode_shard(encoded)!=shard || (i && encoded<=previous)))rc=EFS_ERR_PROTO;
        previous=encoded;
        if(!rc)rc=efs_meta_apply_getattr(&d->kv,n.row.ino,refuse_coord,NULL,&n.stat);
        if(!rc && d->refolded<UINT32_MAX)d->refolded++;
        if(!rc){
            batch[used++]=n;
            /* Sequential candidate rows can share a write. Flush the tail
             * before folds read this file; final publication still fsyncs it. */
            if(used==NODE_BATCH || i+1==d->part[shard].inodes){
                rc=io_at(fd,batch,used*sizeof(*batch),(i+1-used)*sizeof(*batch),1);
                used=0;
            }
        }
    }
    if(rc)fprintf(stderr,"query-disk: nodes shard=%u previous=%llu rc=%d\n",shard,(unsigned long long)previous,rc);
    close(fd);return rc;
}
static void expand(uint64_t *lo,uint64_t *hi,uint64_t l,uint64_t h,int has)
{ if(!has || l<*lo)*lo=l;if(!has || h>*hi)*hi=h; }
static uint64_t low_time(const struct efs_meta_stat *s)
{
    uint64_t t=s->mtime;if(s->ctime<t)t=s->ctime;if(s->atime<t)t=s->atime;return t;
}
static uint64_t high_time(const struct efs_meta_stat *s)
{
    uint64_t t=s->mtime;if(s->ctime>t)t=s->ctime;if(s->atime>t)t=s->atime;return t;
}
static int chosen_shard(struct efs_query_disk *d,const struct efs_meta_row *parent,const struct efs_name_entry *e,uint32_t *chosen)
{
    *chosen=efs_kv_inode_shard(parent->ino);
    if(parent->layout==EFS_META_LAYOUT_LOCAL)return EFS_OK;
    uint32_t hashed=efs_kv_dentry_shard(e->parent,e->name,EFS_META_LAYOUT_HASHED);
    uint8_t key[EFS_KV_KEY_MAX],val[EFS_META_DENT_BYTES];uint32_t kn,vn=sizeof(val);
    int rc=efs_kv_key_dentry(hashed,e->parent,e->name,key,&kn);
    if(!rc)rc=raw_get(d,key,kn,val,&vn);
    if(!rc || parent->layout==EFS_META_LAYOUT_HASHED){*chosen=hashed;return EFS_OK;}
    return rc==EFS_ERR_NOT_FOUND?EFS_OK:rc;
}
static int fold_flush(struct efs_query_disk *d,struct fold_parent *p)
{
    if(!p->loaded)return EFS_OK;
    int rc=node_at(d,efs_kv_inode_shard(p->node.row.ino),p->at,&p->node,1);
    if(!rc)p->loaded=0;
    return rc;
}
static int fold_edge(struct efs_query_disk *d,uint32_t shard,const struct efs_name_entry *e,struct fold_parent *p)
{
    int rc=EFS_OK;
    if(!p->loaded || p->node.row.ino!=e->parent){
        rc=fold_flush(d,p);if(rc)return rc;
        rc=locate(d,e->parent,&p->node,&p->at);if(rc)return rc;
        if(!S_ISDIR(p->node.row.mode))return EFS_ERR_IO;
        p->loaded=1;
    }
    struct disk_node *parent=&p->node,child;uint64_t ca;uint32_t chosen;
    rc=chosen_shard(d,&parent->row,e,&chosen);if(rc)return rc;
    if(chosen!=shard || !e->dentry.ino || e->dentry.type==EFS_META_DENT_TOMBSTONE)return EFS_OK;
    if(e->dentry.ino==e->parent)return EFS_ERR_PROTO;
    rc=locate(d,e->dentry.ino,&child,&ca);if(rc)return rc;
    if(child.row.generation!=e->dentry.generation)return EFS_ERR_IO;
    struct efs_query_rollup *r=&parent->rollup;int has=r->imm_files+r->imm_dirs!=0;
    if(S_ISDIR(child.row.mode)) {
        if(child.link.parent || child.row.parent!=e->parent)return EFS_ERR_PROTO;
        child.link=*e;r->imm_dirs++;parent->children++;
        rc=node_at(d,efs_kv_inode_shard(child.row.ino),ca,&child,1);if(rc)return rc;
    } else {
        if(UINT64_MAX-r->imm_bytes<child.stat.size)return EFS_ERR_QUOTA;
        r->imm_files++;r->imm_bytes+=child.stat.size;
    }
    expand(&r->imm_tmin,&r->imm_tmax,low_time(&child.stat),high_time(&child.stat),has);
    return EFS_OK;
}
static int folds_build(struct efs_query_disk *d)
{
    int rc=EFS_OK;
    /* Raw dentry keys order by parent then name. A single bounded parent
     * accumulator amortizes lookup/writes without changing the name index. */
    for(uint32_t shard=0;shard<SHARDS && !rc;shard++){
        struct fold_parent parent={0};
        for(uint64_t i=0;i<d->part[shard].names && !rc;i++){
            if(cancelled(d))return EFS_ERR_BUSY;
            struct efs_name_entry e={0};struct slot slot;
            uint8_t key[EFS_KV_KEY_MAX],val[VALUE_MAX];
            rc=read_record(d,shard,d->part[shard].begin[1]+i,&slot,key,val);
            if(!rc)rc=entry_from(key,slot.key_len,val,slot.value_len,&e);
            if(!rc)rc=fold_edge(d,shard,&e,&parent);
            if(rc)fprintf(stderr,"query-disk: edge shard=%u parent=%llu child=%llu rc=%d\n",shard,
                (unsigned long long)e.parent,(unsigned long long)e.dentry.ino,rc);
        }
        if(!rc)rc=fold_flush(d,&parent);
    }
    if(rc)return rc;
    char path[EFS_MAX_PATH];int len=snprintf(path,sizeof(path),"%s/queue",d->path);
    if(len<0 || (size_t)len>=sizeof(path))return EFS_ERR_NAMETOOLONG;
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);if(fd<0)return EFS_ERR_IO;
    uint64_t head=0,tail=0,dirs=0;
    for(uint32_t shard=0;shard<SHARDS && !rc;shard++)for(uint64_t i=0;i<d->part[shard].inodes && !rc;i++){
        struct disk_node n;rc=node_at(d,shard,i,&n,0);if(rc)break;
        if(!S_ISDIR(n.row.mode))continue;
        if(n.row.nlink && n.row.ino!=EFS_ROOT_INO && !n.link.parent){
            fprintf(stderr,"query-disk: unlinked directory ino=%llu parent=%llu nlink=%u\n",
                (unsigned long long)n.row.ino,(unsigned long long)n.row.parent,n.row.nlink);
            rc=EFS_ERR_IO;break;
        }
        dirs++;n.rollup.tree_files=n.rollup.imm_files;n.rollup.tree_dirs=n.rollup.imm_dirs;
        n.rollup.tree_bytes=n.rollup.imm_bytes;n.rollup.tree_tmin=n.rollup.imm_tmin;n.rollup.tree_tmax=n.rollup.imm_tmax;
        rc=node_at(d,shard,i,&n,1);
        if(!rc && !n.children)rc=io_at(fd,&n.row.ino,sizeof(n.row.ino),tail++*sizeof(n.row.ino),1);
    }
    while(head<tail && !rc){
        if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
        efs_ino_t ino;rc=io_at(fd,&ino,sizeof(ino),head++*sizeof(ino),0);if(rc)break;
        struct disk_node child,parent;uint64_t at;rc=locate(d,ino,&child,NULL);if(rc)break;
        if(!child.link.parent)continue;
        rc=locate(d,child.link.parent,&parent,&at);if(rc)break;
        struct efs_query_rollup *a=&parent.rollup,*b=&child.rollup;
        int has=a->tree_files+a->tree_dirs!=0;
        if(UINT64_MAX-a->tree_bytes<b->tree_bytes || !parent.children){rc=EFS_ERR_QUOTA;break;}
        a->tree_files+=b->tree_files;a->tree_dirs+=b->tree_dirs;a->tree_bytes+=b->tree_bytes;
        if(b->tree_files+b->tree_dirs)expand(&a->tree_tmin,&a->tree_tmax,b->tree_tmin,b->tree_tmax,has);
        parent.children--;rc=node_at(d,efs_kv_inode_shard(parent.row.ino),at,&parent,1);
        if(!rc && !parent.children)rc=io_at(fd,&parent.row.ino,sizeof(parent.row.ino),tail++*sizeof(parent.row.ino),1);
    }
    close(fd);unlink(path);if(!rc && head!=dirs)rc=EFS_ERR_PROTO;return rc;
}
static int file_digest(int fd,uint8_t out[32])
{
    uint8_t buffer[65536];uint64_t offset=0;blake3_hasher h;blake3_hasher_init(&h);
    for(;;){ssize_t n=pread(fd,buffer,sizeof(buffer),(off_t)offset);
        if(n<0 && errno==EINTR)continue;
        if(n<0)return EFS_ERR_IO;
        if(!n)break;
        blake3_hasher_update(&h,buffer,(size_t)n);offset+=(uint64_t)n;
    }
    blake3_hasher_finalize(&h,out,32);return EFS_OK;
}
/* Private bounded incremental builder; no public/disk ABI change. */
#include "query_disk_incremental.h"
int efs_query_disk_build(const char *root,struct efs_kv_lsm_snapshot *source,
    const struct efs_query_disk *old,uint64_t max_bytes,int (*stop)(void *),void *ctx,struct efs_query_disk **out)
{
    if(out)*out=NULL;
    uint64_t working=sizeof(struct efs_query_disk)+SORT_ROWS*sizeof(struct efs_name_entry)+4*VALUE_MAX+RAW_RECORD_MAX+NODE_BATCH*sizeof(struct disk_node)+sizeof(struct fold_parent)+sizeof(struct incremental)+65536+2*RAW_RECORD_MAX;
    if(!root || !source || !out)return EFS_ERR_INVAL;
    if(max_bytes<working)return EFS_ERR_NOMEM;
    if(mkdir(root,0700) && errno!=EEXIST)return EFS_ERR_IO;
    struct efs_query_disk *d=calloc(1,sizeof(*d));if(!d)return EFS_ERR_NOMEM;
    pthread_once(&reserve_once,configure_reserve);
    d->budget_fd=-1;d->disk_limit=generation_limit;
    d->disk_bytes=sizeof(d->part)+10*sizeof(uint64_t);
    if(d->disk_bytes>d->disk_limit){free(d);return EFS_ERR_QUOTA;}
    d->bytes=working;d->limit=max_bytes;d->stop=stop;d->stop_ctx=ctx;
    d->kv=(struct efs_kv){.ops=&raw_ops,.ctx=d};pthread_mutex_init(&d->mutex,NULL);
    for(int i=0;i<FD_CACHE;i++)d->fds[i].fd=-1;
    int admitted=root_budget_acquire(d,root);
    if(admitted){efs_query_disk_free(d);return admitted;}
    int n=snprintf(d->path,sizeof(d->path),"%s/view-XXXXXX",root);
    if(n<0 || (size_t)n>=sizeof(d->path) || !mkdtemp(d->path)){efs_query_disk_free(d);return EFS_ERR_IO;}
    uint64_t revisions[SHARDS];int rc=efs_kv_lsm_snapshot_revisions(source,revisions,SHARDS);
    for(uint32_t shard=0;shard<SHARDS && !rc;shard++) {
        if(cancelled(d)){rc=EFS_ERR_BUSY;break;}
        d->part[shard].revision=revisions[shard];
        if(old && old->part[shard].revision==revisions[shard]) {
            d->part[shard]=old->part[shard];
            if(old->part[shard].count) {
                char raw[EFS_MAX_PATH];struct stat st;
                rc=filename(old,shard,'r',raw,sizeof(raw));
                if(!rc && stat(raw,&st))rc=EFS_ERR_IO;
                if(!rc)rc=charge_disk(d,(uint64_t)st.st_size+
                    old->part[shard].count*sizeof(struct slot)+
                    derived_charge(old->part[shard].inodes,old->part[shard].names));
            }
            const char files[]={'r','i','n'};
            for(unsigned f=0;f<sizeof(files) && !rc;f++) {
                if(!old->part[shard].count || (files[f]=='n' && !old->part[shard].names))continue;
                char from[EFS_MAX_PATH],to[EFS_MAX_PATH];
                rc=filename(old,shard,files[f],from,sizeof(from));
                if(!rc)rc=filename(d,shard,files[f],to,sizeof(to));
                if(!rc && link(from,to))rc=EFS_ERR_IO;
            }
        } else {
            d->rebuilt++;rc=raw_build(d,source,shard);
            if(rc)fprintf(stderr,"query-disk: raw shard=%u rc=%d errno=%d\n",shard,rc,errno);
            if(!rc) {
                rc=names_build(d,shard);
                if(rc)fprintf(stderr,"query-disk: names shard=%u count=%llu rc=%d errno=%d\n",shard,
                    (unsigned long long)d->part[shard].names,rc,errno);
            }
        }
        if(UINT64_MAX-d->count<d->part[shard].inodes)rc=EFS_ERR_OVERFLOW;
        else d->count+=d->part[shard].inodes;
    }
    if(!rc && old)rc=incremental_build(d,(struct efs_query_disk *)old);
    else if(!rc)rc=INCREMENTAL_FULL;
    if(rc==INCREMENTAL_FULL){
        rc=EFS_OK;
        close_cache(d);memset(d->shared_d,0,sizeof(d->shared_d));d->refolded=0;
        for(uint32_t shard=0;old && shard<SHARDS;shard++){
            char path[EFS_MAX_PATH];rc=filename(d,shard,'d',path,sizeof(path));
            if(rc)break;
            if(unlink(path) && errno!=ENOENT){rc=EFS_ERR_IO;break;}
        }
        for(uint32_t shard=0;shard<SHARDS && !rc;shard++)rc=nodes_build(d,shard);
        if(!rc)rc=folds_build(d);
    }
    close_cache(d);
    /* Derived node files are private to this candidate; fsync before the
     * completed manifest. A failed build never changes the published view. */
    for(uint32_t shard=0;shard<SHARDS && !rc;shard++) {
        const char files[]={'r','i','n','d'};
        for(unsigned i=0;i<sizeof(files) && !rc;i++) {
            if(!d->part[shard].count || (files[i]=='n' && !d->part[shard].names) ||
                (files[i]=='d' && !d->part[shard].inodes))continue;
            /* These immutable inodes were already synced and digested in the
             * old completed generation. Only their new directory links need
             * persistence (the candidate-directory fsync below). Derived rows
             * and every changed raw/index/name file still require both steps. */
            if((i<3 && old && old->part[shard].revision==revisions[shard]) || (i==3 && d->shared_d[shard]))continue;
            char path[EFS_MAX_PATH];rc=filename(d,shard,files[i],path,sizeof(path));if(rc)break;
            int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0){rc=EFS_ERR_IO;break;}
            /* raw_build already checked fsync on the writing descriptors for
             * raw/index; no later stage writes those inodes. Preserve that
             * first barrier (and its writeback error observation), then hash
             * here without syncing the same unchanged bytes a second time. */
            if(i>=2 && fsync(fd))rc=EFS_ERR_IO;
            if(!rc)rc=file_digest(fd,d->part[shard].digest[i]);
            close(fd);
        }
    }
    if(!rc) {
        char path[EFS_MAX_PATH];n=snprintf(path,sizeof(path),"%s/manifest",d->path);
        if(n<0 || (size_t)n>=sizeof(path))rc=EFS_ERR_NAMETOOLONG;
        int fd=rc?-1:open(path,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
        if(fd<0)rc=EFS_ERR_IO;
        uint64_t header[]={UINT64_C(0x454653514449534b),2,sizeof(struct slot),sizeof(struct disk_node),d->count,SHARDS,0,0,0,0};
        if(!rc)rc=io_at(fd,header,sizeof(header),0,1);
        if(!rc)rc=io_at(fd,d->part,sizeof(d->part),sizeof(header),1);
        if(!rc && fsync(fd))rc=EFS_ERR_IO;
        if(fd>=0)close(fd);
        int dir=rc?-1:open(d->path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(!rc && (dir<0 || fsync(dir)))rc=EFS_ERR_IO;
        if(dir>=0)close(dir);
    }
    if(rc){efs_query_disk_free(d);return rc;}
    close(d->budget_fd);d->budget_fd=-1;
    d->stop=NULL;d->stop_ctx=NULL;*out=d;return EFS_OK;
}
uint64_t efs_query_disk_bytes(const struct efs_query_disk *d){return d?d->bytes:0;}
uint32_t efs_query_disk_rebuilt(const struct efs_query_disk *d){return d?d->rebuilt:0;}
uint32_t efs_query_disk_refolded(const struct efs_query_disk *d){return d?d->refolded:0;}
int efs_query_disk_inode(const struct efs_query_disk *cd,efs_ino_t ino,struct efs_meta_row *row,struct efs_meta_stat *stat)
{
    struct disk_node n;int rc=locate((struct efs_query_disk *)cd,ino,&n,NULL);
    if(!rc){if(row)*row=n.row;if(stat)*stat=n.stat;}return rc;
}
int efs_query_disk_stats(const struct efs_query_disk *cd,efs_ino_t ino,struct efs_query_rollup *out)
{
    struct disk_node n;int rc=locate((struct efs_query_disk *)cd,ino,&n,NULL);
    if(!rc && !S_ISDIR(n.row.mode))rc=EFS_ERR_INVAL;
    if(!rc)*out=n.rollup;
    return rc;
}
int efs_query_disk_link(const struct efs_query_disk *cd,efs_ino_t ino,struct efs_name_entry *out)
{
    struct disk_node n;int rc=locate((struct efs_query_disk *)cd,ino,&n,NULL);
    if(!rc && (!S_ISDIR(n.row.mode) || !n.link.parent))rc=EFS_ERR_NOT_FOUND;
    if(!rc)*out=n.link;
    return rc;
}
int efs_query_disk_path(const struct efs_query_disk *d,efs_ino_t dir,efs_ino_t root,char *out,size_t cap)
{
    if(!out || !cap)return EFS_ERR_INVAL;
    size_t len=0;out[0]=0;
    for(uint64_t steps=0;dir!=root && steps<=d->count;steps++) {
        struct efs_name_entry e;int rc=efs_query_disk_link(d,dir,&e);if(rc)return rc;
        size_t nl=strlen(e.name),add=nl+(len?1:0);if(add>=cap-len)return EFS_ERR_NAMETOOLONG;
        memmove(out+add,out,len+1);memcpy(out,e.name,nl);if(len)out[nl]='/';len+=add;dir=e.parent;
    }
    return dir==root?EFS_OK:EFS_ERR_PROTO;
}
static int matches(const char *name,enum efs_name_match match,const char *term)
{
    size_t n=strlen(name),t=strlen(term);
    switch(match){
    case EFS_NAME_EXACT:return !strcmp(name,term);
    case EFS_NAME_PREFIX:return n>=t && !memcmp(name,term,t);
    case EFS_NAME_SUFFIX:return n>=t && !memcmp(name+n-t,term,t);
    case EFS_NAME_CONTAINS:return strstr(name,term)!=NULL;
    default:return 0;
    }
}
static int name_seek(struct efs_query_disk *d,uint32_t shard,const char *term,uint64_t *at)
{
    uint64_t lo=0,hi=d->part[shard].names;
    while(lo<hi){
        uint64_t mid=lo+(hi-lo)/2;struct efs_name_entry e;
        int rc=disk_io(d,shard,'n',&e,sizeof(e),mid*sizeof(e),0);if(rc)return rc;
        if(strcmp(e.name,term)<0)lo=mid+1;else hi=mid;
    }
    *at=lo;return EFS_OK;
}
int efs_query_disk_page(const struct efs_query_disk *cd,efs_ino_t root,enum efs_name_match match,const char *term,int immediate,
    uint64_t *position,uint32_t budget,int (*stop)(void *),void *stop_ctx,
    int (*emit)(void *,const struct efs_name_entry *,const char *),void *user)
{
    if(!cd || !position || !term || !emit || !budget || *position>(UINT64_C(4096)<<32) || match>EFS_NAME_CONTAINS)return EFS_ERR_INVAL;
    struct efs_query_disk *d=(struct efs_query_disk *)cd;struct efs_meta_row row;
    int rc=efs_query_disk_inode(d,root,&row,NULL);if(rc)return rc;if(!S_ISDIR(row.mode))return EFS_ERR_INVAL;
    uint32_t shard=(uint32_t)(*position>>32);uint64_t at=(uint32_t)*position;
    for(;shard<SHARDS;shard++,at=0){
        if(!at && (match==EFS_NAME_EXACT || match==EFS_NAME_PREFIX)){rc=name_seek(d,shard,term,&at);if(rc)return rc;}
        for(;at<d->part[shard].names;at++) {
            *position=((uint64_t)shard<<32)|at;
            if(!budget || (stop && stop(stop_ctx)))return EFS_OK;
            struct efs_name_entry e;rc=disk_io(d,shard,'n',&e,sizeof(e),at*sizeof(e),0);if(rc)return rc;
            budget--;
            if(!matches(e.name,match,term)){
                if(match==EFS_NAME_EXACT || match==EFS_NAME_PREFIX)break;
                continue;
            }
            if(immediate && e.parent!=root)continue;
            char path[EFS_MAX_PATH];rc=efs_query_disk_path(d,e.parent,root,path,sizeof(path));
            if(rc==EFS_ERR_NOT_FOUND)continue;
            if(rc)return rc;
            struct efs_meta_dentry dent;rc=efs_meta_apply_lookup(&d->kv,e.parent,e.name,&dent);
            if(rc==EFS_ERR_NOT_FOUND)continue;
            if(rc)return rc;
            if(!e.dentry.ino || e.dentry.type==EFS_META_DENT_TOMBSTONE || dent.ino!=e.dentry.ino || dent.generation!=e.dentry.generation)continue;
            rc=efs_query_disk_inode(d,e.parent,&row,NULL);if(rc)return rc;
            uint32_t chosen;rc=chosen_shard(d,&row,&e,&chosen);if(rc)return rc;if(chosen!=shard)continue;
            size_t len=strlen(path),nl=strlen(e.name);
            if(len+nl+(len?1:0)>=sizeof(path))return EFS_ERR_NAMETOOLONG;
            if(len)path[len++]='/';
            memcpy(path+len,e.name,nl+1);
            rc=emit(user,&e,path);if(rc==EFS_ERR_AGAIN || rc==EFS_ERR_BUSY)return EFS_OK;if(rc)return rc;
        }
        *position=(uint64_t)(shard+1)<<32;
    }
    *position=UINT64_C(4096)<<32;return EFS_OK;
}
struct bounded_scan { struct efs_query_disk *d;uint64_t remaining;int (*stop)(void *);void *stop_ctx;
    int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t);void *user; };
static int bounded_record(void *ctx,const uint8_t *k,uint32_t kn,const uint8_t *v,uint32_t vn)
{
    struct bounded_scan *s=ctx;if(s->stop && s->stop(s->stop_ctx))return EFS_ERR_BUSY;
    if(!s->remaining)return EFS_ERR_OVERFLOW;
    s->remaining--;return s->cb(s->user,k,kn,v,vn);
}
static int bounded_get(void *ctx,const uint8_t *k,uint32_t kn,uint8_t *v,uint32_t *vn)
{ return raw_get(((struct bounded_scan *)ctx)->d,k,kn,v,vn); }
static int bounded_from(void *ctx,const uint8_t *p,uint32_t pn,const uint8_t *k,uint32_t kn,
    int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *user)
{
    struct bounded_scan *s=ctx;s->cb=cb;s->user=user;
    return raw_scan(s->d,p,pn,k,kn,bounded_record,s);
}
static int bounded_prefix(void *ctx,const uint8_t *p,uint32_t pn,
    int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *user)
{ return bounded_from(ctx,p,pn,NULL,0,cb,user); }
static const struct efs_kv_ops bounded_ops={.get=bounded_get,.scan_prefix=bounded_prefix,.scan_from=bounded_from};
int efs_query_disk_ls(const struct efs_query_disk *cd,efs_ino_t root,uint64_t budget,int (*stop)(void *),void *stop_ctx,
    int (*emit)(void *,const struct efs_name_entry *,const char *),void *user)
{
    struct efs_meta_row row;int rc=efs_query_disk_inode(cd,root,&row,NULL);if(rc)return rc;
    if(!S_ISDIR(row.mode))return EFS_ERR_INVAL;
    struct bounded_scan scan={.d=(struct efs_query_disk *)cd,.remaining=budget,.stop=stop,.stop_ctx=stop_ctx};
    struct efs_kv kv={.ops=&bounded_ops,.ctx=&scan};struct efs_meta_dir_cursor cursor={0};
    while(!cursor.done) {
        struct efs_meta_dir_ent entries[64];uint32_t n=0;
        rc=efs_meta_apply_readdir(&kv,root,&cursor,entries,64,&n);if(rc)return rc;
        for(uint32_t i=0;i<n;i++){
            struct efs_name_entry e={0};e.parent=root;e.dentry=entries[i].d;memcpy(e.name,entries[i].name,sizeof(e.name));
            rc=emit(user,&e,e.name);if(rc)return rc;
        }
    }
    return EFS_OK;
}
int efs_query_disk_checkpoint(struct efs_query_disk *d,const uint64_t watermark[4])
{
    if(!d || !watermark)return EFS_ERR_INVAL;
    char manifest[EFS_MAX_PATH];int n=snprintf(manifest,sizeof(manifest),"%s/manifest",d->path);
    if(n<0 || (size_t)n>=sizeof(manifest))return EFS_ERR_NAMETOOLONG;
    int fd=open(manifest,O_RDWR|O_CLOEXEC);if(fd<0)return EFS_ERR_IO;
    int rc=io_at(fd,(void *)watermark,4*sizeof(uint64_t),6*sizeof(uint64_t),1);
    if(!rc && fsync(fd))rc=EFS_ERR_IO;
    close(fd);if(rc)return rc;
    const char *base=strrchr(d->path,'/');if(!base)return EFS_ERR_INVAL;
    char current[EFS_MAX_PATH],temp[EFS_MAX_PATH];
    n=snprintf(current,sizeof(current),"%.*s/current",(int)(base-d->path),d->path);
    if(n<0 || (size_t)n>=sizeof(current))return EFS_ERR_NAMETOOLONG;
    n=snprintf(temp,sizeof(temp),"%s.new",current);
    if(n<0 || (size_t)n>=sizeof(temp))return EFS_ERR_NAMETOOLONG;
    unlink(temp);if(symlink(base+1,temp))return EFS_ERR_IO;
    if(rename(temp,current)){unlink(temp);return EFS_ERR_IO;}
    char root[EFS_MAX_PATH];memcpy(root,d->path,(size_t)(base-d->path));root[base-d->path]=0;
    fd=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return EFS_ERR_IO;
    if(fsync(fd))rc=EFS_ERR_IO;
    close(fd);
    if(!rc)memcpy(d->watermark,watermark,sizeof(d->watermark));
    return rc;
}
int efs_query_disk_restore(const char *root,const uint64_t watermark[4],uint64_t max_bytes,struct efs_query_disk **out)
{
    if(out)*out=NULL;
    if(!root || !watermark || !out)return EFS_ERR_INVAL;
    uint64_t bytes=sizeof(struct efs_query_disk)+SORT_ROWS*sizeof(struct efs_name_entry)+4*VALUE_MAX+RAW_RECORD_MAX+NODE_BATCH*sizeof(struct disk_node)+sizeof(struct fold_parent)+sizeof(struct incremental)+65536+2*RAW_RECORD_MAX;
    if(max_bytes<bytes)return EFS_ERR_NOMEM;
    char current[EFS_MAX_PATH],base[256];int n=snprintf(current,sizeof(current),"%s/current",root);
    if(n<0 || (size_t)n>=sizeof(current))return EFS_ERR_NAMETOOLONG;
    ssize_t length=readlink(current,base,sizeof(base)-1);
    if(length<0)return errno==ENOENT?EFS_ERR_NOT_FOUND:EFS_ERR_IO;
    base[length]=0;if(strncmp(base,"view-",5) || strchr(base,'/'))return EFS_ERR_PROTO;
    struct efs_query_disk *d=calloc(1,sizeof(*d));if(!d)return EFS_ERR_NOMEM;
    d->budget_fd=-1;
    pthread_mutex_init(&d->mutex,NULL);for(int i=0;i<FD_CACHE;i++)d->fds[i].fd=-1;
    pthread_once(&reserve_once,configure_reserve);
    d->disk_limit=generation_limit;d->disk_bytes=sizeof(d->part)+10*sizeof(uint64_t);
    d->bytes=bytes;d->limit=max_bytes;d->kv=(struct efs_kv){.ops=&raw_ops,.ctx=d};
    n=snprintf(d->path,sizeof(d->path),"%s/%s",root,base);
    if(n<0 || (size_t)n>=sizeof(d->path)){efs_query_disk_free(d);return EFS_ERR_NAMETOOLONG;}
    char manifest[EFS_MAX_PATH];n=snprintf(manifest,sizeof(manifest),"%s/manifest",d->path);
    int fd=n>0 && (size_t)n<sizeof(manifest)?open(manifest,O_RDONLY|O_CLOEXEC):-1;
    uint64_t header[10];int rc=fd<0?EFS_ERR_IO:io_at(fd,header,sizeof(header),0,0);
    if(!rc && d->disk_bytes>d->disk_limit)rc=EFS_ERR_QUOTA;
    if(!rc && (header[0]!=UINT64_C(0x454653514449534b) || header[1]!=2 || header[2]!=sizeof(struct slot) ||
       header[3]!=sizeof(struct disk_node) || header[5]!=SHARDS))rc=EFS_ERR_PROTO;
    if(!rc && memcmp(header+6,watermark,4*sizeof(uint64_t)))rc=EFS_ERR_STALE;
    if(!rc)rc=io_at(fd,d->part,sizeof(d->part),sizeof(header),0);
    if(fd>=0)close(fd);
    uint64_t count=0;
    for(uint32_t shard=0;shard<SHARDS && !rc;shard++) {
        struct disk_part *p=&d->part[shard];
        if(p->count>UINT32_MAX || p->inodes>p->count || p->names>p->count){rc=EFS_ERR_PROTO;break;}
        for(int i=0;i<KINDS;i++)if(p->begin[i]>p->end[i] || p->end[i]>p->count || (i && p->begin[i]!=p->end[i-1]))rc=EFS_ERR_PROTO;
        if(rc)break;
        count+=p->inodes;const char kinds_file[]={'r','i','n','d'};
        for(unsigned i=0;i<sizeof(kinds_file) && !rc;i++){
            if(!p->count || (kinds_file[i]=='n' && !p->names) || (kinds_file[i]=='d' && !p->inodes))continue;
            char path[EFS_MAX_PATH];rc=filename(d,shard,kinds_file[i],path,sizeof(path));if(rc)break;
            fd=open(path,O_RDONLY|O_CLOEXEC);uint8_t digest[32];struct stat st;
            if(fd<0 || fstat(fd,&st))rc=EFS_ERR_IO;
            uint64_t expected=i==1?p->count*sizeof(struct slot):i==2?p->names*sizeof(struct efs_name_entry):i==3?p->inodes*sizeof(struct disk_node):0;
            if(!rc && expected && (uint64_t)st.st_size!=expected)rc=EFS_ERR_PROTO;
            if(!rc && i==0)rc=charge_disk(d,(uint64_t)st.st_size+p->count*sizeof(struct slot)+
                derived_charge(p->inodes,p->names));
            if(!rc)rc=file_digest(fd,digest);
            if(!rc && memcmp(digest,p->digest[i],sizeof(digest)))rc=EFS_ERR_IO;
            if(fd>=0)close(fd);
        }
    }
    if(!rc && count!=header[4])rc=EFS_ERR_PROTO;
    if(rc){efs_query_disk_free(d);return rc;}
    d->count=count;memcpy(d->watermark,watermark,sizeof(d->watermark));*out=d;return EFS_OK;
}

int efs_query_disk_rebase(struct efs_query_disk *d,struct efs_kv_lsm_snapshot *source)
{
    uint64_t revisions[SHARDS];int rc=efs_kv_lsm_snapshot_revisions(source,revisions,SHARDS);
    if(!rc)for(unsigned i=0;i<SHARDS;i++)d->part[i].revision=revisions[i];
    return rc;
}
