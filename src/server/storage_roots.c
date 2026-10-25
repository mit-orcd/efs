/* Storage roots: exclusive root locks, the root format marker and a
 * pinned-inode ancestry barrier for namespace durability. */
#include "storage_roots.h"
#include "efs/checksum.h"
#include "efs/store.h"
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
#define DURABLE_DIRS 128u
struct target {
    pthread_mutex_t durable_mu;
    struct { int fd; dev_t dev; ino_t ino; uint64_t touched; } durable[DURABLE_DIRS];
    uint64_t durable_clock;
    unsigned durable_slots;
    char root[4096];
    int lockfd, rootfd;
    uint8_t uuid[16];
};
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
/* A pinned directory descriptor prevents inode-number reuse from turning a
 * cache hit into a durability guess. Entries are admitted only after the full
 * ancestry barrier succeeds. GC may remove directories: a newly created one
 * cannot match the pinned, unlinked inode. Capacity pressure evicts a hint. */
static int durable_known(struct target *t, const struct stat *st)
{
    int known=0;
    pthread_mutex_lock(&t->durable_mu);
    for(unsigned i=0;i<t->durable_slots;i++)if(t->durable[i].fd>=0 &&
        t->durable[i].dev==st->st_dev && t->durable[i].ino==st->st_ino){
        t->durable[i].touched=++t->durable_clock;known=1;break;
    }
    pthread_mutex_unlock(&t->durable_mu);
    return known;
}
static void durable_remember(struct target *t,int fd,const struct stat *st)
{
    unsigned slot=0;int dispose=-1;
    pthread_mutex_lock(&t->durable_mu);
    for(unsigned i=0;i<t->durable_slots;i++){
        if(t->durable[i].fd>=0 && t->durable[i].dev==st->st_dev &&
           t->durable[i].ino==st->st_ino){
            t->durable[i].touched=++t->durable_clock;dispose=fd;goto done;
        }
        if(t->durable[i].fd<0){slot=i;break;}
        if(t->durable[i].touched<t->durable[slot].touched)slot=i;
    }
    dispose=t->durable[slot].fd;
    t->durable[slot].fd=fd;t->durable[slot].dev=st->st_dev;
    t->durable[slot].ino=st->st_ino;t->durable[slot].touched=++t->durable_clock;
done:
    pthread_mutex_unlock(&t->durable_mu);
    if(dispose>=0)close(dispose);
}
/* Startup seals the root path. Runtime publication syncs the changed
 * namespace and any unknown ancestry to a pinned durability boundary or the
 * root itself, never unrelated ancestors. */
static int parents_at_sync(const char *path, struct target *t,
                           int (*sync)(void *,int),void *context)
{
    const char *root=t?t->root:NULL;
    if (root && strcmp(root, "/") &&
        (strncmp(path, root, strlen(root)) || path[strlen(root)] != '/'))
        return EFS_ERR_IO;
    char p[8192];
    if(strlen(path)>=sizeof(p))return EFS_ERR_IO;
    strcpy(p,path);
    char *slash=strrchr(p,'/');
    if(!slash)return EFS_ERR_IO;
    if(slash==p)slash[1]=0;
    else *slash=0;
    int retained[16];unsigned retained_n=0;
    struct stat retained_st[16];
    for(;;){
        int is_root=t && !strcmp(p,t->root);
        int fd=is_root?t->rootfd:t?openat(t->rootfd,p+strlen(t->root)+1,O_RDONLY|O_DIRECTORY|O_CLOEXEC):open(p,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        int rc=EFS_OK, known=0;
        if(fd<0){
            if(errno!=ENOENT){rc=EFS_ERR_IO;goto failed;}
        }
        else{
            struct stat st;
            if(t && !is_root){
                if(fstat(fd,&st)){close(fd);rc=EFS_ERR_IO;goto failed;}
                known=durable_known(t,&st);
                if(!known && retained_n<16){
                    int pin=fcntl(fd,F_DUPFD_CLOEXEC,0);
                    if(pin>=0){retained[retained_n]=pin;retained_st[retained_n++]=st;}
                }
            }
            /* A hit elides only unchanged ancestry. The namespace operation's
             * containing directory still needs a fresh barrier on EVERY call. */
            rc=sync?sync(context,fd):barrier(fd);
            if(!is_root && close(fd))rc=EFS_ERR_IO;
            if(rc)goto failed;
        }
        if(known || !strcmp(p,"/") || (root && !strcmp(p,root)))break;
        slash=strrchr(p,'/');
        if(!slash){rc=EFS_ERR_IO;goto failed;}
        if(slash==p)slash[1]=0;
        else *slash=0;
        continue;
failed:
        for(unsigned i=0;i<retained_n;i++)close(retained[i]);
        return rc;
    }
    /* Remember every successfully sealed ancestor, not just the temporary
     * per-inode leaf: sibling publication/deletion can reuse shared ancestry.
     * The fixed LRU descriptor budget preserves hot shared parents despite
     * inode-number/hash collisions; no failed barrier admits any entry. */
    for(unsigned i=retained_n;i>0;i--)durable_remember(t,retained[i-1],&retained_st[i-1]);
    return EFS_OK;
}
static int parents_at(const char *path,struct target *t)
{ return parents_at_sync(path,t,NULL,NULL); }
static int parents(const char *path) { return parents_at(path, NULL); }
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
/* Root markers are published once through a mkstemp temporary. */
static int temp_open(const char *path,char tmp[8192])
{
    int n=snprintf(tmp,8192,"%s.tmp.XXXXXX",path);
    if(n>=8192){errno=ENAMETOOLONG;return -1;}
    return mkstemp(tmp);
}
static int atomic_page(const char *path,const uint8_t *page,size_t n,int *published)
{
    char tmp[8192];
    *published=0;
    int fd=temp_open(path,tmp);
    if(fd<0 && errno==ENOENT) {
        if(mkdirs(path))return EFS_ERR_IO;
        fd=temp_open(path,tmp);
    }
    if(fd<0)return EFS_ERR_IO;
    int rc=full_io(fd,(void*)page,n,0,1);
    if(!rc)rc=barrier(fd);
    if(close(fd))rc=EFS_ERR_IO;
    if(!rc){
        int rr=rename(tmp,path);
        if(rr)rc=EFS_ERR_IO;
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
    if(rc||memcmp(page,"EFSENG01",8)||memcmp(page+96,h,32))return EFS_ERR_IO;
    return EFS_OK;
}
int server_storage_root_mode(const char *root)
{
    uint8_t page[128]={0};
    int rc=marker_read(root,page);
    /* A marker of any other root format is refused, like a corrupt one. */
    return rc==EFS_ERR_NOT_FOUND?0:rc?-1:page[8]==EFS_STORAGE_ROOT_FORMAT?page[8]:-1;
}
static struct target *target_at(struct efsd_server *s,const char *path)
{
    struct target *best=NULL;size_t longest=0;
    for(unsigned i=0;i<s->storage_path_count;i++){
        struct target *t=s->storage_roots[i];
        if(!t)continue;
        size_t n=strlen(t->root);
        /* Files can have nested targets; match the same longest canonical
         * root as PUT routing, rather than an earlier outer target. */
        if(n>longest && !strncmp(path,t->root,n) && path[n]=='/'){
            best=t;longest=n;
        }
    }
    return best;
}
/* Pinned-inode ancestry barrier for GC: every call still syncs the changed
 * containing directory, with GC's syscall accounting and failure
 * propagation through its own directory barrier. */
int server_storage_sync_parents_with(struct efsd_server *s,const char *path,
                                    int (*sync)(void *,int),void *context)
{
    struct target *t=target_at(s,path);
    return t?parents_at_sync(path,t,sync,context):EFS_ERR_IO;
}
int server_storage_metadata_path(const char *relative)
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
static void root_put32(uint8_t *p,uint32_t v){for(int i=3;i>=0;i--){p[i]=(uint8_t)v;v>>=8;}}
static uint32_t root_get32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
int server_storage_roots_init(struct efsd_server *s)
{
    uint8_t markers[EFS_MAX_STORAGE_PATHS][128];
    unsigned fresh=0;
    if(!s->storage_path_count || s->storage_path_count>EFS_MAX_STORAGE_PATHS)return EFS_ERR_INVAL;
    memset(markers,0,sizeof(markers));
    for(unsigned i=0;i<s->storage_path_count;i++) {
        char canonical[4096];
        if(!realpath(s->storage_paths[i],canonical) || strlen(canonical)>=sizeof(s->storage_paths[i]))goto fail;
        strcpy(s->storage_paths[i],canonical);
    }
    for(unsigned i=0;i<s->storage_path_count;i++){
        struct target *t=calloc(1,sizeof(*t));
        if(!t)goto fail;
        s->storage_roots[i]=t;
        t->lockfd=t->rootfd=-1;
        pthread_mutex_init(&t->durable_mu,NULL);
        /* Bound retained hints across the whole fixed target set, not per
         * root: normal sockets/metadata still need descriptor headroom. */
        t->durable_slots=DURABLE_DIRS/s->storage_path_count;
        for(unsigned j=0;j<DURABLE_DIRS;j++)t->durable[j].fd=-1;
        if (!realpath(s->storage_paths[i], t->root)) goto fail;
        if (strlen(t->root) >= sizeof(s->storage_paths[i])) goto fail;
        strcpy(s->storage_paths[i], t->root);
        size_t n=strlen(t->root);
        while(n>1&&t->root[n-1]=='/')t->root[--n]=0;
        char p[8192];
        snprintf(p,sizeof(p),"%s/.efs-engine-lock",t->root);
        t->lockfd=open(p,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        if(t->lockfd<0||flock(t->lockfd,LOCK_EX|LOCK_NB))goto fail;
        t->rootfd=open(t->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(t->rootfd<0)goto fail;
        int rc=marker_read(t->root,markers[i]);
        if(rc==EFS_ERR_NOT_FOUND){
            /* No marker: a fresh root, unless fragment data already exists
             * (written by a build that predates markers, format unknown). */
            /* efsd creates an empty data/ before this; any entry in it is
             * data of an unknown format. */
            snprintf(p,sizeof(p),"%s/data",t->root);
            DIR *dd=opendir(p);
            int has_data=0;
            if(dd){
                struct dirent *de;
                while((de=readdir(dd)))if(strcmp(de->d_name,".")&&strcmp(de->d_name,"..")){has_data=1;break;}
                closedir(dd);
            } else if(errno!=ENOENT)goto fail;
            if(has_data){
                fprintf(stderr,"storage: %s has fragment data but no root marker; "
                        "its format is unknown and this build reads only root format %u "
                        "(fragment record format 2: I25 identity-bound records); refusing\n",
                        t->root,(unsigned)EFS_STORAGE_ROOT_FORMAT);
                goto fail;
            }
            fresh++;
        }
        else if(rc)goto fail;
        else if(markers[i][8]!=EFS_STORAGE_ROOT_FORMAT){
            /* No migration and no legacy reader (not in production). */
            fprintf(stderr,"storage: %s has root format %u (%s); this build reads "
                    "only root format %u (fragment format 2: I25 identity-bound "
                    "fragment records); refusing startup, reformat the root\n",
                    t->root,(unsigned)markers[i][8],
                    markers[i][8]==0?"fragment format 1: payload-only digest tail":"unknown",
                    (unsigned)EFS_STORAGE_ROOT_FORMAT);
            goto fail;
        }
    }
    /* F11: a node's roots are fixed, in order, when they are first marked.
     * Fragments are found by probing every root, but packed segments are
     * placed by id modulo the root count, so a root added, dropped or moved
     * later would hide data. The marker records the set (root 0's uuid),
     * this root's index and the count; a different set is refused. */
    unsigned n=s->storage_path_count;
    if(fresh && fresh!=n){
        fprintf(stderr,"storage: %u of %u roots are new; a node's storage roots are fixed "
                "when its store is created (give the same roots, in the same order, or format "
                "all of them as a new store)\n",fresh,n);
        goto fail;
    }
    for(unsigned i=0;i<n && fresh;i++){
        memcpy(markers[i],"EFSENG01",8);
        markers[i][8]=EFS_STORAGE_ROOT_FORMAT;
        if(getrandom(markers[i]+16,16,0)!=16)goto fail;
        memcpy(markers[i]+32,markers[0]+16,16);
        root_put32(markers[i]+48,i);
        root_put32(markers[i]+52,n);
        efs_hash(markers[i],96,markers[i]+96);
    }
    for(unsigned i=0;i<n;i++){
        const uint8_t *m=markers[i];
        if(!root_get32(m+52)){
            fprintf(stderr,"storage: %s has a root marker without a root set (an earlier "
                    "build); format a new store\n",s->storage_paths[i]);
            goto fail;
        }
        if(memcmp(m+32,markers[0]+16,16) || root_get32(m+48)!=i || root_get32(m+52)!=n){
            fprintf(stderr,"storage: %s is root %u of %u of its root set; this command "
                    "gives it as root %u of %u. A node's storage roots are fixed when its "
                    "store is created: give the same roots, in the same order\n",
                    s->storage_paths[i],root_get32(m+48)+1,root_get32(m+52),i+1,n);
            goto fail;
        }
    }
    for(unsigned i=0;i<n;i++){
        struct target *t=s->storage_roots[i];
        char p[8192];
        snprintf(p,sizeof(p),"%s/.efs-engine",t->root);
        int published;
        if(fresh && atomic_page(p,markers[i],128,&published))goto fail;
        /* Existing roots may have been configured at a new pathname; persist
         * that ancestry once before bounded runtime namespace barriers. */
        if(parents(p))goto fail;
        memcpy(t->uuid,markers[i]+16,16);
        for(unsigned j=0;j<i;j++)if(!memcmp(((struct target*)s->storage_roots[j])->uuid,t->uuid,16))goto fail;
    }
    strcpy(s->storage_path, s->storage_paths[0]);
    return EFS_OK;
    fail:fprintf(stderr,"storage: incompatible, busy or corrupt root; refusing startup\n");
    server_storage_roots_close(s);
    return EFS_ERR_IO;
}
void server_storage_roots_close(struct efsd_server *s)
{
    for(unsigned i=0;i<EFS_MAX_STORAGE_PATHS;i++){
        struct target *t=s->storage_roots[i];
        if(!t)continue;
        if(t->rootfd>=0)close(t->rootfd);
        if(t->lockfd>=0)close(t->lockfd);
        for(unsigned j=0;j<DURABLE_DIRS;j++)if(t->durable[j].fd>=0)close(t->durable[j].fd);
        pthread_mutex_destroy(&t->durable_mu);
        free(t);
        s->storage_roots[i]=NULL;
    }
}

