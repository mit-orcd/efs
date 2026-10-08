/* Experimental mapping-file container engine. Filesystem namespace is the
* durable index; allocation bitmaps are reconstructed before admission.
* No acknowledged mapping is reclaimed using age, header scans or guesses. */
#include "store_containers.h"
#include "efs/checksum.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#define PAGE 4096u
#define SEG_BYTES (UINT64_C(1)<<30)
#define BLOCKS (SEG_BYTES/PAGE)
#define MAX_SEGS 1024u
struct target {
    pthread_mutex_t mu;
    char root[4096], dir[8192];
    int lockfd, failed;
    unsigned cursor_seg, cursor_block;
    uint8_t uuid[16];
    uint8_t *bits[MAX_SEGS];
}
;
struct mapping {
    uint8_t page[PAGE];
    uint32_t seg, len, span, off;
}
;
static void put32(uint8_t *p,uint32_t v){
    for(int i=3;i>=0;i--){
        p[i]=(uint8_t)v;
        v>>=8;
    }
}
static uint32_t get32(const uint8_t *p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static int full_io(int fd,void *buf,size_t len,off_t off,int writing)
{
    size_t done=0;
    while(done<len){
        ssize_t n=writing?pwrite(fd,(char*)buf+done,len-done,off+done):pread(fd,(char*)buf+done,len-done,off+done);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return EFS_ERR_IO;
        done+=(size_t)n;
    }
    return EFS_OK;
}
static int barrier(int fd){
    int r;
    do{
        r=fsync(fd);
    }
    while(r&&errno==EINTR);
    return r?EFS_ERR_IO:EFS_OK;
}
static int parents(const char *path)
{
    char p[8192];
    if(strlen(path)>=sizeof(p))return EFS_ERR_IO;
    strcpy(p,path);
    char *slash=strrchr(p,'/');
    if(!slash)return EFS_ERR_IO;
    if(slash==p)slash[1]=0;
    else *slash=0;
    for(;;){
        int fd=open(p,O_RDONLY|O_DIRECTORY|O_CLOEXEC),rc=EFS_OK;
        if(fd<0){
            if(errno!=ENOENT)return EFS_ERR_IO;
        }
        else{
            rc=barrier(fd);
            if(close(fd))rc=EFS_ERR_IO;
            if(rc)return rc;
        }
        if(!strcmp(p,"/"))break;
        slash=strrchr(p,'/');
        if(!slash)return EFS_ERR_IO;
        if(slash==p)slash[1]=0;
        else *slash=0;
    }
    return EFS_OK;
}
static int mkdirs(const char *path)
{
    char p[8192];
    if(strlen(path)>=sizeof(p))return EFS_ERR_IO;
    strcpy(p,path);
    for(char *q=p+1;*q;q++)if(*q=='/'){
        *q=0;
        if(mkdir(p,0755)&&errno!=EEXIST)return EFS_ERR_IO;
        *q='/';
    }
    return EFS_OK;
}
static int atomic_page(const char *path,const uint8_t *page,size_t n,int *published)
{
    char tmp[8192];
    *published=0;
    if(mkdirs(path))return EFS_ERR_IO;
    if(snprintf(tmp,sizeof(tmp),"%s.tmp.XXXXXX",path)>=(int)sizeof(tmp))return EFS_ERR_IO;
    int fd=mkstemp(tmp);
    if(fd<0)return EFS_ERR_IO;
    int rc=full_io(fd,(void*)page,n,0,1);
    if(!rc)rc=barrier(fd);
    if(close(fd))rc=EFS_ERR_IO;
    if(!rc){
        if(rename(tmp,path))rc=EFS_ERR_IO;
        else{
            *published=1;
            rc=parents(path);
        }
    }
    if(!*published)unlink(tmp);
    return rc;
}
static int marker_read(const char *root,uint8_t page[128])
{
    char path[8192];
    snprintf(path,sizeof(path),"%s/.efs-engine",root);
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return errno==ENOENT?EFS_ERR_NOT_FOUND:EFS_ERR_IO;
    struct stat st;
    int rc=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=128?EFS_ERR_IO:full_io(fd,page,128,0,0);
    if(close(fd))rc=EFS_ERR_IO;
    uint8_t h[32];
    efs_hash(page,96,h);
    if(rc||memcmp(page,"EFSENG01",8)||page[8]>1||memcmp(page+96,h,32))return EFS_ERR_IO;
    return EFS_OK;
}
int server_storage_root_mode(const char *root)
{
    uint8_t page[128]={
        0}
    ;
    int rc=marker_read(root,page);
    return rc==EFS_ERR_NOT_FOUND?0:rc?-1:page[8];
}
static int directory_empty(const char *path)
{
    DIR *d=opendir(path);
    if(!d)return errno==ENOENT?1:-1;
    int empty=1;
    struct dirent *e;
    errno=0;
    while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")){
        empty=0;
        break;
    }
    if(!e&&errno)empty=-1;
    if(closedir(d))empty=-1;
    return empty;
}
static struct target *target_at(struct efsd_server *s,const char *path)
{
    for(unsigned i=0;i<s->storage_path_count;i++){
        struct target *t=s->container_targets[i];
        if(!t)continue;
        size_t n=strlen(t->root);
        if(!strncmp(path,t->root,n)&&path[n]=='/')return t;
    }
    return NULL;
}
static void segpath(struct target *t,unsigned seg,char path[8192]){
    snprintf(path,8192,"%s/%016x.dat",t->dir,seg);
}
static int marked(struct target *t,unsigned seg,unsigned block){
    return (t->bits[seg][block/8]>>(block%8))&1;
}
static void mark(struct target *t,unsigned seg,unsigned off,unsigned span,int busy)
{
    for(unsigned b=off;b<off+span;b++){
        uint8_t mask=(uint8_t)(1u<<(b%8));
        if(busy)t->bits[seg][b/8]|=mask;
        else t->bits[seg][b/8]&=(uint8_t)~mask;
    }
}
static int decode(const char *path,struct target *t,struct mapping *m)
{
    uint8_t h[32];
    efs_hash(m->page,PAGE-32,h);
    if(memcmp(m->page,"EFSCMAP1",8)||memcmp(h,m->page+PAGE-32,32))return EFS_ERR_IO;
    m->seg=get32(m->page+32);
    m->off=get32(m->page+36);
    m->span=get32(m->page+40);
    m->len=get32(m->page+44);
    if(m->seg>=MAX_SEGS||m->len>EFS_MAX_FRAGMENT_SIZE||m->span!=1+(m->len+PAGE-1)/PAGE||m->off>BLOCKS-m->span||m->page[48]>1)return EFS_ERR_IO;
    if(t){
        if(memcmp(t->uuid,m->page+8,16))return EFS_ERR_IO;
        efs_hash(path+strlen(t->root),strlen(path+strlen(t->root)),h);
        if(memcmp(h,m->page+96,32))return EFS_ERR_IO;
    }
    return EFS_OK;
}
static int mapping_read(const char *path,struct target *t,struct mapping *m)
{
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return errno==ENOENT?EFS_ERR_NOT_FOUND:EFS_ERR_IO;
    struct stat st;
    int rc=fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=PAGE?EFS_ERR_IO:full_io(fd,m->page,PAGE,0,0);
    if(close(fd))rc=EFS_ERR_IO;
    return rc?rc:decode(path,t,m);
}
int server_container_payload(const char *path,uint64_t *len)
{
    struct mapping m;
    int rc=mapping_read(path,NULL,&m);
    if(!rc)*len=m.len;
    return rc;
}
int server_container_info(struct efsd_server *s,const char *path,uint32_t *len,uint8_t *sum)
{
    struct target *t=target_at(s,path);
    if(!t)return EFS_ERR_IO;
    struct mapping m;
    int rc=mapping_read(path,t,&m);
    if(!rc){
        *len=m.len;
        if(sum)memcpy(sum,m.page+64,32);
    }
    return rc;
}
static int segment_create(struct target *t,unsigned seg)
{
    char path[8192],tmp[8192];
    segpath(t,seg,path);
    snprintf(tmp,sizeof(tmp),"%s/.new.XXXXXX",t->dir);
    int fd=mkstemp(tmp);
    if(fd<0)return EFS_ERR_IO;
    int rc=ftruncate(fd,SEG_BYTES)?EFS_ERR_IO:barrier(fd);
    if(close(fd))rc=EFS_ERR_IO;
    if(!rc&&link(tmp,path))rc=EFS_ERR_IO;
    unlink(tmp);
    if(!rc)rc=parents(path);
    if(!rc){
        t->bits[seg]=calloc(1,BLOCKS/8);
        if(!t->bits[seg])rc=EFS_ERR_NOMEM;
    }
    if(rc)t->failed=1;
    return rc;
}
static int reserve(struct target *t,struct mapping *m)
{
    pthread_mutex_lock(&t->mu);
    int rc=EFS_ERR_IO;
    if(t->failed)goto done;
    for(unsigned si=0;si<MAX_SEGS;si++){
        unsigned seg=(t->cursor_seg+si)%MAX_SEGS;
        if(!t->bits[seg]&&(rc=segment_create(t,seg)))goto done;
        unsigned start=si==0?t->cursor_block:0;
        for(unsigned pass=0;pass<2;pass++) {
            unsigned lo=pass?0:start, hi=pass?start:BLOCKS, run=0;
            for(unsigned b=lo;b<hi;b++){
                run=marked(t,seg,b)?0:run+1;
                if(run==m->span){
                    m->seg=seg;m->off=b+1-run;
                    mark(t,seg,m->off,m->span,1);
                    t->cursor_seg=seg;t->cursor_block=b+1;
                    if(t->cursor_block==BLOCKS)t->cursor_block=0;
                    rc=EFS_OK;goto done;
                }
            }
        }
    }
    rc=EFS_ERR_QUOTA;
    done:pthread_mutex_unlock(&t->mu);
    return rc;
}
static int release_extent(struct target *t,struct mapping *m)
{
    char path[8192];
    segpath(t,m->seg,path);
    int fd=open(path,O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return EFS_ERR_IO;
    int rc=fallocate(fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)m->off*PAGE,(off_t)m->span*PAGE)?EFS_ERR_IO:barrier(fd);
    if(close(fd))rc=EFS_ERR_IO;
    if(!rc){
        pthread_mutex_lock(&t->mu);
        mark(t,m->seg,m->off,m->span,0);
        pthread_mutex_unlock(&t->mu);
    }
    return rc;
}
int server_container_put(struct efsd_server *s,const char *path,const uint8_t *data,uint32_t len,const uint8_t *sum,int *published)
{
    *published=0;
    if(len>EFS_MAX_FRAGMENT_SIZE||(len&&!data))return EFS_ERR_INVAL;
    struct target *t=target_at(s,path);
    if(!t)return EFS_ERR_IO;
    struct mapping old,m={0};
    int found=mapping_read(path,t,&old);
    if(found!=EFS_OK&&found!=EFS_ERR_NOT_FOUND)return found;
    m.len=len;
    m.span=1+(len+PAGE-1)/PAGE;
    int rc=reserve(t,&m);
    if(rc)return rc;
    memcpy(m.page,"EFSCMAP1",8);
    memcpy(m.page+8,t->uuid,16);
    if(getrandom(m.page+24,8,0)!=8)return EFS_ERR_IO;
    /* retain reserved range on failure */
    put32(m.page+32,m.seg);
    put32(m.page+36,m.off);
    put32(m.page+40,m.span);
    put32(m.page+44,len);
    m.page[48]=sum?1:0;
    efs_hash(data,len,m.page+64);
    if(sum&&memcmp(sum,m.page+64,32))return EFS_ERR_IO;
    efs_hash(path+strlen(t->root),strlen(path+strlen(t->root)),m.page+96);
    efs_hash(m.page,PAGE-32,m.page+PAGE-32);
    uint8_t *buf=NULL;
    size_t bytes=(size_t)m.span*PAGE;
    if(posix_memalign((void**)&buf,PAGE,bytes))return EFS_ERR_NOMEM;
    memset(buf,0,bytes);
    memcpy(buf,m.page,PAGE);
    if(len)memcpy(buf+PAGE,data,len);
    char body[8192];
    segpath(t,m.seg,body);
    int fd=open(body,O_RDWR|O_CLOEXEC|O_NOFOLLOW|(s->direct_io?O_DIRECT:0));
    rc=fd<0?EFS_ERR_IO:full_io(fd,buf,bytes,(off_t)m.off*PAGE,1);
    if(fd>=0){
        if(!rc)rc=barrier(fd);
        if(close(fd))rc=EFS_ERR_IO;
    }
    free(buf);
    if(rc)return rc;
    /* incomplete/uncertain I/O: quarantine until restart */
    rc=atomic_page(path,m.page,PAGE,published);
    if(rc)return rc;
    /* even a failed directory barrier can expose the new map */
    if(found==EFS_OK){
        /* Mapping is durable. Failure to reclaim old space must not undo it.
        * Busy bits retain failed retirements until startup reconciliation. */
        if(release_extent(t,&old))fprintf(stderr,"containers: retained old extent after punch/sync failure\n");
    }
    return EFS_OK;
}
int server_container_get(struct efsd_server *s,const char *path,uint8_t *data,uint32_t *len,uint8_t *sum,int *sum_ok)
{
    struct target *t=target_at(s,path);
    if(!t)return EFS_ERR_IO;
    struct mapping m;
    int rc=mapping_read(path,t,&m);
    if(rc)return rc;
    uint8_t *buf=NULL;
    size_t bytes=(size_t)m.span*PAGE;
    if(posix_memalign((void**)&buf,PAGE,bytes))return EFS_ERR_NOMEM;
    char body[8192];
    segpath(t,m.seg,body);
    int fd=open(body,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|(s->direct_io?O_DIRECT:0));
    rc=fd<0?EFS_ERR_IO:full_io(fd,buf,bytes,(off_t)m.off*PAGE,0);
    if(fd>=0&&close(fd))rc=EFS_ERR_IO;
    uint8_t hash[32];
    if(!rc){
        efs_hash(buf+PAGE,m.len,hash);
        if(memcmp(buf,m.page,PAGE)||memcmp(hash,m.page+64,32))rc=EFS_ERR_IO;
    }
    if(!rc){
        uint32_t n=*len<m.len?*len:m.len;
        memcpy(data,buf+PAGE,n);
        *len=n;
        if(sum)memcpy(sum,m.page+64,32);
        if(sum_ok)*sum_ok=m.page[48];
    }
    free(buf);
    return rc;
}
int server_container_delete(struct efsd_server *s,const char *path,const uint8_t *expect,uint64_t *payload,uint64_t *bytes)
{
    *payload=*bytes=0;
    struct target *t=target_at(s,path);
    if(!t)return EFS_ERR_IO;
    struct mapping m;
    int rc=mapping_read(path,t,&m);
    if(rc==EFS_ERR_NOT_FOUND)return parents(path);
    if(rc)return rc;
    if(expect&&memcmp(expect,m.page+64,32))return EFS_ERR_EXIST;
    if(unlink(path))return EFS_ERR_IO;
    rc=parents(path);
    if(rc)return rc;
    *payload=m.len;
    *bytes=(uint64_t)m.span*PAGE+PAGE;
    /* Unlinked mapping is now durable; physical release cannot race readers
    * because the caller holds the fragment/inode lifetime lock. */
    return release_extent(t,&m);
}
int server_container_metadata_path(const char *relative)
{
    if (strncmp(relative, "/data/exports/", 14)) return 0;
    const char *p = relative + 14, *eid = p;
    while (*p >= '0' && *p <= '9') p++;
    if (p == eid || *p++ != '/') return 0;
    uint64_t ino = 0;
    for (unsigned group = 0; group < EFS_INO_PATH_SEGS; group++) {
        unsigned value = 0;
        for (unsigned digit = 0; digit < 4; digit++) {
            if (*p < '0' || *p > '9') return 0;
            value = value * 10 + (unsigned)(*p++ - '0');
        }
        if (ino > (UINT64_MAX - value) / 10000) return 0;
        ino = ino * 10000 + value;
        if (group < EFS_INO_PATH_SEGS - 1) { if (*p++ != '/') return 0; }
        else if (*p && *p != '/') return 0;
    }
    return efs_ino_is_meta_table(ino);
}
static int recover_maps(struct target *t,const char *path,unsigned depth)
{
    if(depth>16)return EFS_ERR_IO;
    DIR *d=opendir(path);
    if(!d)return errno==ENOENT?EFS_OK:EFS_ERR_IO;
    struct dirent *e;
    int rc=EFS_OK;
    for(;;){
        errno=0;
        e=readdir(d);
        if(!e){
            if(errno)rc=EFS_ERR_IO;
            break;
        }
        if(e->d_name[0]=='.')continue;
        char child[8192];
        if(snprintf(child,sizeof(child),"%s/%s",path,e->d_name)>=(int)sizeof(child)){
            rc=EFS_ERR_IO;
            break;
        }
        struct stat st;
        if(lstat(child,&st)){
            rc=EFS_ERR_IO;
            break;
        }
        if(server_container_metadata_path(child+strlen(t->root)))continue;
        if(S_ISDIR(st.st_mode))rc=recover_maps(t,child,depth+1);
        else if(S_ISREG(st.st_mode)){
            /* Atomic replacement temporaries are never authoritative. A crash
            * leaves them unreachable; remove and sync before freeing ranges. */
            if(strstr(e->d_name,".tmp.")){
                if(unlink(child))rc=EFS_ERR_IO;
            }
            else if(st.st_size==PAGE){
                struct mapping m;
                rc=mapping_read(child,t,&m);
                if(!rc){
                    if(!t->bits[m.seg])rc=EFS_ERR_IO;
                    else{
                        for(unsigned b=m.off;b<m.off+m.span;b++)if(marked(t,m.seg,b)){
                            rc=EFS_ERR_IO;
                            break;
                        }
                        if(!rc) {
                        char body[8192];uint8_t header[PAGE];segpath(t,m.seg,body);
                        int fd=open(body,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
                        rc=fd<0?EFS_ERR_IO:full_io(fd,header,PAGE,(off_t)m.off*PAGE,0);
                        if(fd>=0&&close(fd))rc=EFS_ERR_IO;
                        if(!rc&&memcmp(header,m.page,PAGE))rc=EFS_ERR_IO;
                        if(!rc)mark(t,m.seg,m.off,m.span,1);
                    }
                    }
                }
            }
            else rc=EFS_ERR_IO;
        }
        else rc=EFS_ERR_IO;
        if(rc)break;
    }
    if(!rc)rc=barrier(dirfd(d));
    if(closedir(d))rc=EFS_ERR_IO;
    return rc;
}
static int load_segments(struct target *t)
{
    DIR *d=opendir(t->dir);
    if(!d)return EFS_ERR_IO;
    int rc=EFS_OK;
    struct dirent *e;
    for(;;){
        errno=0;
        e=readdir(d);
        if(!e){
            if(errno)rc=EFS_ERR_IO;
            break;
        }
        if(e->d_name[0]=='.'){
            if(!strncmp(e->d_name,".new.",5)){
                char p[8192];
                snprintf(p,sizeof(p),"%s/%s",t->dir,e->d_name);
                if(unlink(p))rc=EFS_ERR_IO;
            }
            continue;
        }
        unsigned seg;
        int used=0;
        if(sscanf(e->d_name,"%16x.dat%n",&seg,&used)!=1||e->d_name[used]||seg>=MAX_SEGS||t->bits[seg]){
            rc=EFS_ERR_IO;
            break;
        }
        char p[8192];
        segpath(t,seg,p);
        struct stat st;
        if(lstat(p,&st)||!S_ISREG(st.st_mode)||st.st_size!=(off_t)SEG_BYTES){
            rc=EFS_ERR_IO;
            break;
        }
        t->bits[seg]=calloc(1,BLOCKS/8);
        if(!t->bits[seg]){
            rc=EFS_ERR_NOMEM;
            break;
        }
    }
    if(closedir(d))rc=EFS_ERR_IO;
    return rc;
}
static int recover_free(struct target *t)
{
    for(unsigned seg=0;seg<MAX_SEGS;seg++)if(t->bits[seg]){
        char p[8192];
        segpath(t,seg,p);
        int fd=open(p,O_RDWR|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0)return EFS_ERR_IO;
        int rc=EFS_OK;
        unsigned b=0;
        while(b<BLOCKS){
            if(marked(t,seg,b)){
                b++;
                continue;
            }
            unsigned first=b;
            while(b<BLOCKS&&!marked(t,seg,b))b++;
            if(fallocate(fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)first*PAGE,(off_t)(b-first)*PAGE)){
                rc=EFS_ERR_IO;
                break;
            }
        }
        if(!rc)rc=barrier(fd);
        if(close(fd))rc=EFS_ERR_IO;
        if(rc)return rc;
    }
    return EFS_OK;
}
int server_storage_engine_init(struct efsd_server *s)
{
    for(unsigned i=0;i<s->storage_path_count;i++){
        struct target *t=calloc(1,sizeof(*t));
        if(!t)goto fail;
        s->container_targets[i]=t;
        t->lockfd=-1;
        pthread_mutex_init(&t->mu,NULL);
        if (!realpath(s->storage_paths[i], t->root)) goto fail;
        if (strlen(t->root) >= sizeof(s->storage_paths[i])) goto fail;
        strcpy(s->storage_paths[i], t->root);
        size_t n=strlen(t->root);
        while(n>1&&t->root[n-1]=='/')t->root[--n]=0;
        snprintf(t->dir,sizeof(t->dir),"%s/.efs-containers",t->root);
        char p[8192];
        snprintf(p,sizeof(p),"%s/.efs-engine-lock",t->root);
        t->lockfd=open(p,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        if(t->lockfd<0||flock(t->lockfd,LOCK_EX|LOCK_NB))goto fail;
        uint8_t marker[128]={0};
        int rc=marker_read(t->root,marker);
        if(rc==EFS_ERR_NOT_FOUND){
            char data[8192];
            snprintf(data,sizeof(data),"%s/data",t->root);
            if(s->storage_engine&&directory_empty(data)!=1)goto fail;
            if(!s->storage_engine&&directory_empty(t->dir)!=1)goto fail;
            memcpy(marker,"EFSENG01",8);
            marker[8]=(uint8_t)s->storage_engine;
            if(getrandom(marker+16,16,0)!=16)goto fail;
            efs_hash(marker,96,marker+96);
            snprintf(p,sizeof(p),"%s/.efs-engine",t->root);
            int published;
            if(atomic_page(p,marker,128,&published))goto fail;
        }
        else if(rc||marker[8]!=s->storage_engine)goto fail;
        memcpy(t->uuid,marker+16,16);
        for(unsigned j=0;j<i;j++)if(!memcmp(((struct target*)s->container_targets[j])->uuid,t->uuid,16))goto fail;
        if(s->storage_engine){
            if(mkdir(t->dir,0755)&&errno!=EEXIST)goto fail;
            if(parents(t->dir))goto fail;
            if(load_segments(t))goto fail;
            snprintf(p,sizeof(p),"%s/data",t->root);
            if(recover_maps(t,p,0)||recover_free(t))goto fail;
        }
    }
    strcpy(s->storage_path, s->storage_paths[0]);
    return EFS_OK;
    fail:fprintf(stderr,"storage-engine: incompatible, busy or corrupt root; refusing startup\n");
    server_storage_engine_close(s);
    return EFS_ERR_IO;
}
void server_storage_engine_close(struct efsd_server *s)
{
    for(unsigned i=0;i<EFS_MAX_STORAGE_PATHS;i++){
        struct target *t=s->container_targets[i];
        if(!t)continue;
        if(t->lockfd>=0)close(t->lockfd);
        for(unsigned j=0;j<MAX_SEGS;j++)free(t->bits[j]);
        pthread_mutex_destroy(&t->mu);
        free(t);
        s->container_targets[i]=NULL;
    }
}

/* Dynamic addition retains files-engine compatibility and exclusive root locks.
 * Containers intentionally require a fixed target set for this first format. */
int server_storage_files_attach(struct efsd_server *s, const char *root, unsigned index)
{
    if (s->storage_engine || index >= EFS_MAX_STORAGE_PATHS || s->container_targets[index]) return EFS_ERR_INVAL;
    struct efsd_server *one = calloc(1, sizeof(*one));
    if (!one) return EFS_ERR_NOMEM;
    one->storage_path_count = 1;
    if (strlen(root) >= sizeof(one->storage_paths[0])) { free(one); return EFS_ERR_INVAL; }
    strcpy(one->storage_paths[0], root);
    int rc = server_storage_engine_init(one);
    if (!rc) {
        s->container_targets[index] = one->container_targets[0];
        one->container_targets[0] = NULL;
        strcpy(s->storage_paths[index], one->storage_paths[0]);
    }
    free(one);
    return rc;
}
