/* Exercise the production cache boundary and partial-write error behavior. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/ioctl.h>
static int clone_error;
static int fault_clone(int fd,unsigned long request,int source)
{ if(clone_error){errno=clone_error;return -1;}return ioctl(fd,request,source); }
#endif
static int partial_write;
static unsigned write_calls;
static unsigned read_calls;
static unsigned raw_syncs,index_syncs,sync_faults;
static char fail_sync_kind,fail_write_kind;
static unsigned write_faults,fail_derived_after;
static char descriptor_kind[4096];
static int tracked_open(const char *path,int flags,...)
{
    mode_t mode=0;
    if(flags&O_CREAT){va_list args;va_start(args,flags);mode=va_arg(args,int);va_end(args);}
    int fd=open(path,flags,mode);
    if(fd>=0 && fd<(int)sizeof(descriptor_kind)){
        size_t n=strlen(path);char kind=0;
        if(!(flags&O_DIRECTORY)){
            if(n>=2 && path[n-2]=='.')kind=path[n-1];
            const char *name=strrchr(path,'/');if(name && !strcmp(name+1,"manifest"))kind='m';
        }
        descriptor_kind[fd]=kind;
    }
    return fd;
}
static int fault_fsync(int fd)
{
    char kind=fd>=0 && fd<(int)sizeof(descriptor_kind)?descriptor_kind[fd]:0;
    if(kind=='r')raw_syncs++;
    if(kind=='i')index_syncs++;
    if(kind && kind==fail_sync_kind){sync_faults++;errno=EIO;return -1;}
    return fsync(fd);
}
static ssize_t counted_pread(int fd,void *buf,size_t size,off_t offset)
{ read_calls++;return pread(fd,buf,size,offset); }
static ssize_t fault_pwrite(int fd,const void *buf,size_t size,off_t offset)
{
    write_calls++;
    if(fail_derived_after && fd>=0 && fd<(int)sizeof(descriptor_kind) && descriptor_kind[fd]=='d' && !--fail_derived_after){
        write_faults++;partial_write=1;
    }
    if(fail_write_kind && fd>=0 && fd<(int)sizeof(descriptor_kind) && descriptor_kind[fd]==fail_write_kind){
        fail_write_kind=0;write_faults++;partial_write=1;
    }
    if(partial_write==2){partial_write=0;errno=ENOSPC;return -1;}
    if(partial_write==1){partial_write=2;return pwrite(fd,buf,size<3?size:3,offset);}
    return pwrite(fd,buf,size,offset);
}
#ifdef __linux__
#define ioctl fault_clone
#endif
#define pwrite fault_pwrite
#define pread counted_pread
#define fsync fault_fsync
#define open tracked_open
#include "../src/meta/query_disk.c"
#undef pwrite
#undef pread
#undef fsync
#undef open
#ifdef __linux__
#undef ioctl
#endif
static int failures;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);failures++;}}while(0)
/* Exercise the full-rebuild boundary after a partial attempt has retained
 * directory rows with already-folded links. Replacing .d files must make
 * subsequent lookups read the new zeroed rows, not those retained links. */
static void rebuild_cache_test(void)
{
    char store[]="/tmp/efs-query-rebuild-kv-XXXXXX",cache[]="/tmp/efs-query-rebuild-cache-XXXXXX";
    CHECK(mkdtemp(store));CHECK(mkdtemp(cache));
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};
    struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);CHECK(kv);if(!kv)return;
    struct efs_meta_attrs attrs={1000,1000,100};efs_ino_t dir,file;
    CHECK(!efs_meta_apply_init(kv,100));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"dir",&dir));
    CHECK(!efs_meta_apply_create_file(kv,&attrs,dir,S_IFREG|0644,"file",&file));
    struct efs_kv_lsm_snapshot *source=NULL;struct efs_query_disk *disk=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,&disk));
    if(disk){
        struct disk_node retained;CHECK(!locate(disk,dir,&retained,NULL));
        CHECK(retained.link.parent==EFS_ROOT_INO);
        close_cache(disk);
        for(unsigned shard=0;shard<SHARDS;shard++){
            char path[EFS_MAX_PATH];CHECK(!filename(disk,shard,'d',path,sizeof(path)));
            CHECK(!unlink(path) || errno==ENOENT);
            CHECK(!nodes_build(disk,shard));
        }
        CHECK(!folds_build(disk));
        struct efs_query_rollup totals;CHECK(!efs_query_disk_stats(disk,EFS_ROOT_INO,&totals));
        CHECK(totals.tree_files==1 && totals.tree_dirs==1);
    }
    efs_query_disk_free(disk);efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
}
static int cancel_budget(void *ctx){(void)ctx;return 1;}
static void budget_test(void)
{
    char root[]="/tmp/efs-query-budget-XXXXXX",a[EFS_MAX_PATH],b[EFS_MAX_PATH],c[EFS_MAX_PATH],path[EFS_MAX_PATH];
    CHECK(mkdtemp(root));
    snprintf(a,sizeof(a),"%s/view-a",root);snprintf(b,sizeof(b),"%s/view-b",root);snprintf(c,sizeof(c),"%s/view-orphan",root);
    CHECK(!mkdir(a,0700));CHECK(!mkdir(b,0700));CHECK(!mkdir(c,0700));
    char source[EFS_MAX_PATH];snprintf(source,sizeof(source),"%s/part",a);
    int fd=open(source,O_CREAT|O_RDWR,0600);CHECK(fd>=0);if(fd>=0){CHECK(!ftruncate(fd,4096));close(fd);}
    snprintf(path,sizeof(path),"%s/part",b);CHECK(!link(source,path));
    snprintf(path,sizeof(path),"%s/partial.copy",c);fd=open(path,O_CREAT|O_RDWR,0600);CHECK(fd>=0);
    if(fd>=0){CHECK(!ftruncate(fd,4096));close(fd);}
    struct efs_query_disk *q=calloc(1,sizeof(*q)),*other=calloc(1,sizeof(*other));CHECK(q && other);
    if(q && other){
        uint64_t saved=aggregate_limit;aggregate_limit=3*4096+255;
        q->budget_fd=other->budget_fd=-1;q->disk_limit=other->disk_limit=UINT64_MAX;q->disk_bytes=256;
        CHECK(root_budget_acquire(q,root)==EFS_ERR_QUOTA && q->budget_fd==-1);
        aggregate_limit++;
        CHECK(!root_budget_acquire(q,root) && q->root_bytes==3*4096);
        CHECK(root_budget_acquire(other,root)==EFS_ERR_BUSY);
        CHECK(charge_disk(q,1)==EFS_ERR_QUOTA && q->disk_bytes==256);
        close(q->budget_fd);q->budget_fd=-1;q->stop=cancel_budget;
        CHECK(root_budget_acquire(q,root)==EFS_ERR_BUSY && q->budget_fd==-1);
        CHECK(!root_budget_acquire(other,root));close(other->budget_fd);other->budget_fd=-1;
        /* A lease release reduces the reconstructed total on the next build. */
        CHECK(!unlink(path));CHECK(!rmdir(c));q->stop=NULL;
        CHECK(!root_budget_acquire(q,root) && q->root_bytes==2*4096);
        CHECK(!charge_disk(q,4096));CHECK(charge_disk(q,1)==EFS_ERR_QUOTA);
        close(q->budget_fd);q->budget_fd=-1;
        snprintf(path,sizeof(path),"%s/journal",root);fd=open(path,O_CREAT|O_RDWR,0600);CHECK(fd>=0);
        if(fd>=0){CHECK(!ftruncate(fd,8192));close(fd);}
        CHECK(root_budget_acquire(q,root)==EFS_ERR_QUOTA);CHECK(!unlink(path));
        CHECK(!symlink(source,path));CHECK(root_budget_acquire(q,root)==EFS_ERR_PROTO);CHECK(!unlink(path));
        CHECK(!root_budget_acquire(q,root));close(q->budget_fd);q->budget_fd=-1;
        aggregate_limit=saved;
    }
    free(q);free(other);unlink(source);rmdir(a);
    snprintf(path,sizeof(path),"%s/part",b);unlink(path);rmdir(b);
    snprintf(path,sizeof(path),"%s/.budget.lock",root);unlink(path);rmdir(root);
}
int main(void)
{
    rebuild_cache_test();
    {
        struct efs_query_rollup r={.tree_files=UINT64_MAX-1,.tree_dirs=7,.tree_bytes=9};
        CHECK(!rollup_tree_add(&r,1,2,3));
        struct efs_query_rollup before=r;
        CHECK(rollup_tree_add(&r,1,0,0)==EFS_ERR_OVERFLOW);
        CHECK(!memcmp(&r,&before,sizeof(r)));
        r.tree_dirs=UINT64_MAX;before=r;
        CHECK(rollup_tree_add(&r,0,1,0)==EFS_ERR_OVERFLOW);
        CHECK(!memcmp(&r,&before,sizeof(r)));
        r.tree_bytes=UINT64_MAX;before=r;
        CHECK(rollup_tree_add(&r,0,0,1)==EFS_ERR_OVERFLOW);
        CHECK(!memcmp(&r,&before,sizeof(r)));
    }

    budget_test();
    struct efs_query_disk *d=calloc(1,sizeof(*d));if(!d)return 1;
    d->budget_fd=-1;
    d->disk_limit=UINT64_MAX;
    strcpy(d->path,"/tmp/efs-query-window-XXXXXX");
    if(!mkdtemp(d->path)){free(d);return 1;}
    pthread_mutex_init(&d->mutex,NULL);
    for(int i=0;i<FD_CACHE;i++)d->fds[i].fd=-1;
    uint8_t data[READ_WINDOW*3+17],got[READ_WINDOW*2],change[31];
    for(size_t i=0;i<sizeof(data);i++)data[i]=(uint8_t)(i*17+i/251);
    memset(change,0xa7,sizeof(change));
    int fd=make_file(d,0,'r');CHECK(fd>=0);
    if(fd>=0){CHECK(!io_at(fd,data,sizeof(data),0,1));close(fd);}
    const uint64_t offsets[]={0,READ_WINDOW-7,READ_WINDOW+11,sizeof(data)-17};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++){
        CHECK(!disk_io(d,0,'r',got,17,offsets[i],0));CHECK(!memcmp(got,data+offsets[i],17));
    }
    CHECK(!disk_io(d,0,'r',got,sizeof(got),13,0));CHECK(!memcmp(got,data+13,sizeof(got)));
    CHECK(disk_io(d,0,'r',got,18,sizeof(data)-17,0)==EFS_ERR_IO);
    CHECK(disk_io(d,0,'r',got,2,INT64_MAX,0)==EFS_ERR_OVERFLOW);
    /* Four disjoint blocks remain hot while binary searches alternate.
     * Warm all ways, then prove repeats issue no reads (including EOF tail). */
    for(unsigned i=0;i<READ_WAYS;i++)CHECK(!disk_io(d,0,'r',got,17,i*READ_WINDOW,0));
    read_calls=0;
    for(unsigned i=0;i<READ_WAYS*3;i++){
        size_t off=(i%READ_WAYS)*READ_WINDOW;
        CHECK(!disk_io(d,0,'r',got,17,off,0));CHECK(!memcmp(got,data+off,17));
    }
    CHECK(read_calls==0);
    /* Warm a window, write inside it, then write across its end. */
    CHECK(!disk_io(d,0,'r',got,17,20,0));
    CHECK(!disk_io(d,0,'r',change,sizeof(change),23,1));memcpy(data+23,change,sizeof(change));
    CHECK(!disk_io(d,0,'r',got,17,20,0));CHECK(!memcmp(got,data+20,17));
    CHECK(!disk_io(d,0,'r',change,sizeof(change),READ_WINDOW-9,1));memcpy(data+READ_WINDOW-9,change,sizeof(change));
    CHECK(!disk_io(d,0,'r',got,17,READ_WINDOW-17,0));CHECK(!memcmp(got,data+READ_WINDOW-17,17));
    /* The failed write changed three disk bytes: cached old bytes must vanish. */
    CHECK(!disk_io(d,0,'r',got,17,100,0));partial_write=1;
    CHECK(disk_io(d,0,'r',change,10,100,1)==EFS_ERR_IO);memcpy(data+100,change,3);
    CHECK(!disk_io(d,0,'r',got,17,100,0));CHECK(!memcmp(got,data+100,17));
    /* Both resident blocks overlapping a short failed write must be
     * invalidated, including the block containing its changed prefix tail. */
    CHECK(!disk_io(d,0,'r',got,17,READ_WINDOW-17,0));
    CHECK(!disk_io(d,0,'r',got,17,READ_WINDOW,0));partial_write=1;
    CHECK(disk_io(d,0,'r',change,10,READ_WINDOW-2,1)==EFS_ERR_IO);
    memcpy(data+READ_WINDOW-2,change,3);
    CHECK(!disk_io(d,0,'r',got,17,READ_WINDOW-17,0));CHECK(!memcmp(got,data+READ_WINDOW-17,17));
    CHECK(!disk_io(d,0,'r',got,17,READ_WINDOW,0));CHECK(!memcmp(got,data+READ_WINDOW,17));
    /* Extending the cached short EOF block exposes the newly written bytes. */
    CHECK(!disk_io(d,0,'r',got,17,READ_WINDOW*3,0));
    CHECK(!disk_io(d,0,'r',change,sizeof(change),sizeof(data)-5,1));
    memcpy(data+sizeof(data)-5,change,5);
    CHECK(!disk_io(d,0,'r',got,sizeof(change),sizeof(data)-5,0));CHECK(!memcmp(got,change,sizeof(change)));
    /* Extending in another block fills the cached EOF gap with zeros. */
    CHECK(!disk_io(d,0,'r',change,17,READ_WINDOW*4,1));
    CHECK(!disk_io(d,0,'r',got,17,READ_WINDOW*3+100,0));
    for(unsigned i=0;i<17;i++)CHECK(got[i]==0);
    CHECK(!disk_io(d,0,'r',got,17,READ_WINDOW*4,0));CHECK(!memcmp(got,change,17));
    /* Exact median lookup needs one cold window; gaps and end misses must
     * still report NOT_FOUND rather than returning a neighboring inode. */
    struct disk_node nodes[31]={0},found;uint64_t at=UINT64_MAX;
    for(unsigned i=0;i<31;i++)nodes[i].row.ino=(uint64_t)(i+1)*8192;
    d->part[0].inodes=31;fd=make_file(d,0,'d');CHECK(fd>=0);
    if(fd>=0){CHECK(!io_at(fd,nodes,sizeof(nodes),0,1));close(fd);}
    read_calls=0;CHECK(!locate(d,16*8192,&found,&at));
    CHECK(found.row.ino==16*8192 && at==15 && read_calls==1);
    read_calls=0;CHECK(!locate(d,16*8192,&found,&at) && !read_calls);
    found.stat.size=123;CHECK(!node_at(d,0,15,&found,1));
    CHECK(!locate(d,16*8192,&found,&at) && found.stat.size==123);
    d->inode_epoch=UINT64_MAX;
    found.stat.size=456;CHECK(!node_at(d,0,15,&found,1));
    CHECK(!locate(d,16*8192,&found,&at) && found.stat.size==456 && d->inode_epoch==UINT64_MAX);
    CHECK(locate(d,0,&found,NULL)==EFS_ERR_NOT_FOUND);
    CHECK(locate(d,3*4096,&found,NULL)==EFS_ERR_NOT_FOUND);
    CHECK(locate(d,32*8192,&found,NULL)==EFS_ERR_NOT_FOUND);
    /* Unique raw keys: exact search reuses its slot; absent and prefix
     * searches preserve lower-bound scan semantics and value capacity. */
    struct writer search={.d=d,.shard=98,.raw=-1,.index=-1};
    d->disk_limit=UINT64_MAX;
    uint8_t search_key[11]={0,98,EFS_KV_KIND_INODE},search_value=0;
    for(unsigned i=0;i<31;i++){
        search_key[10]=(uint8_t)(2*i+2);search_value=(uint8_t)i;
        CHECK(!save(&search,search_key,sizeof(search_key),&search_value,1));
    }
    close(search.raw);close(search.index);
    d->part[98].count=31;d->part[98].end[0]=31;
    struct slot exact={0};uint64_t lower=UINT64_MAX;
    search_key[10]=32;read_calls=0;
    CHECK(!seek(d,search_key,sizeof(search_key),&lower,&exact));
    CHECK(lower==15 && exact.key_len==11 && read_calls==4);
    uint32_t capacity=0;CHECK(raw_get(d,search_key,11,got,&capacity)==EFS_ERR_INVAL && capacity==1);
    capacity=1;CHECK(!raw_get(d,search_key,11,got,&capacity) && got[0]==15);
    search_key[10]=31;capacity=sizeof(got);
    CHECK(raw_get(d,search_key,11,got,&capacity)==EFS_ERR_NOT_FOUND);
    CHECK(!seek(d,search_key,11,&lower,NULL) && lower==15);
    search_key[10]=0;CHECK(!seek(d,search_key,11,&lower,NULL) && lower==0);
    search_key[10]=63;CHECK(!seek(d,search_key,11,&lower,NULL) && lower==31);
    CHECK(!seek(d,search_key,3,&lower,NULL) && lower==0);
    /* Raw record assembly must preserve bytes and never add an index entry
     * after a short write followed by ENOSPC. It uses two writes, not three. */
    uint8_t key[11]={0,99,EFS_KV_KIND_INODE},value[37],record[48];
    memset(value,0x39,sizeof(value));d->disk_limit=UINT64_MAX;
    struct writer writer={.d=d,.shard=99,.raw=-1,.index=-1};
    write_calls=0;CHECK(!save(&writer,key,sizeof(key),value,sizeof(value)));
    CHECK(write_calls==2 && writer.count==1 && writer.offset==sizeof(record));
    CHECK(!io_at(writer.raw,record,sizeof(record),0,0));
    CHECK(!memcmp(record,key,sizeof(key)) && !memcmp(record+sizeof(key),value,sizeof(value)));
    partial_write=1;CHECK(save(&writer,key,sizeof(key),value,sizeof(value))==EFS_ERR_IO);
    CHECK(writer.count==1 && writer.offset==sizeof(record));
    struct stat index_stat;CHECK(!fstat(writer.index,&index_stat));
    CHECK(index_stat.st_size==(off_t)sizeof(struct slot));
    CHECK(!save(&writer,key,sizeof(key),NULL,0));CHECK(writer.count==2);
    close(writer.raw);close(writer.index);
    /* Reusing every FD slot must not reuse bytes from its previous file. */
    for(uint32_t shard=1;shard<=FD_CACHE+2;shard++){
        uint8_t byte=(uint8_t)shard;fd=make_file(d,shard,'r');CHECK(fd>=0);
        if(fd>=0){CHECK(!io_at(fd,&byte,1,0,1));close(fd);}
        CHECK(!disk_io(d,shard,'r',got,1,0,0));CHECK(got[0]==byte);
        CHECK(disk_io(d,shard,'r',got,17,READ_WINDOW,0)==EFS_ERR_IO);
    }
    CHECK(!disk_io(d,0,'r',got,17,100,0));CHECK(!memcmp(got,data+100,17));
    /* A failed open must not leave the evicted key pointing to a reused FD. */
    CHECK(disk_io(d,SHARDS-1,'r',got,1,0,0)==EFS_ERR_IO);
    CHECK(!d->fd_index[SHARDS-1][fd_kind('r')]);
    CHECK(!disk_io(d,FD_CACHE+2,'r',got,1,0,0));CHECK(got[0]==FD_CACHE+2);
    CHECK(disk_io(d,SHARDS,'r',got,1,0,0)==EFS_ERR_INVAL);
    CHECK(disk_io(d,0,'x',got,1,0,0)==EFS_ERR_INVAL);
    close_cache(d);
    for(unsigned i=0;i<SHARDS;i++)for(unsigned k=0;k<4;k++)CHECK(!d->fd_index[i][k]);
    CHECK(!disk_io(d,0,'r',got,17,100,0));CHECK(!memcmp(got,data+100,17));
    /* Copy-on-write must detach the cached descriptor and preserve the old
     * inode even when the candidate write is short and fails with ENOSPC. */
    char retained[]="/tmp/efs-query-retained-XXXXXX",derived[EFS_MAX_PATH];
    int oldfd=mkstemp(retained);CHECK(oldfd>=0);
    if(oldfd>=0){
        CHECK(!io_at(oldfd,data,sizeof(data),0,1));
        CHECK(!filename(d,SHARDS-2,'d',derived,sizeof(derived)));CHECK(!link(retained,derived));d->shared_d[SHARDS-2]=1;
        CHECK(!disk_io(d,SHARDS-2,'d',got,17,100,0));CHECK(!memcmp(got,data+100,17));
        uint64_t admitted=d->disk_bytes;
        d->disk_limit=admitted+sizeof(data)-1;
        CHECK(disk_io(d,SHARDS-2,'d',change,sizeof(change),100,1)==EFS_ERR_QUOTA);
        CHECK(d->shared_d[SHARDS-2] && d->disk_bytes==admitted);d->disk_limit=UINT64_MAX;
#ifdef __linux__
        clone_error=EIO;
        CHECK(disk_io(d,SHARDS-2,'d',change,sizeof(change),100,1)==EFS_ERR_IO);
        CHECK(d->shared_d[SHARDS-2] && d->disk_bytes==admitted);
        clone_error=EOPNOTSUPP; /* Explicitly exercise the non-reflink copy. */
#endif
        partial_write=1;CHECK(disk_io(d,SHARDS-2,'d',change,sizeof(change),100,1)==EFS_ERR_IO);
        CHECK(pread(oldfd,got,17,100)==17);CHECK(!memcmp(got,data+100,17));
        CHECK(!disk_io(d,SHARDS-2,'d',change,sizeof(change),100,1));
        CHECK(!disk_io(d,SHARDS-2,'d',got,sizeof(change),100,0));CHECK(!memcmp(got,change,sizeof(change)));
        CHECK(pread(oldfd,got,17,100)==17);CHECK(!memcmp(got,data+100,17));close(oldfd);unlink(retained);
#ifdef __linux__
        clone_error=0;
#endif
    }
    efs_query_disk_free(d);
    /* A new raw/index inode has one checked writing-handle barrier. Inject
     * failures in every persisted file class; none may return a candidate or
     * replace the checkpoint of a previously completed generation. */
    char kvroot[]="/tmp/efs-query-sync-kv-XXXXXX",cache[]="/tmp/efs-query-sync-cache-XXXXXX";
    CHECK(mkdtemp(kvroot));CHECK(mkdtemp(cache));
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};
    struct efs_kv *kv=efs_kv_lsm_open(kvroot,&cfg);CHECK(kv);
    struct efs_kv_lsm_snapshot *source=NULL;struct efs_query_disk *old=NULL,*next=NULL;
    if(kv){
        struct efs_meta_attrs attrs={1000,1000,100};efs_ino_t file;
        CHECK(!efs_meta_apply_init(kv,100));
        CHECK(!efs_meta_apply_create_file(kv,&attrs,EFS_ROOT_INO,S_IFREG|0644,"file",&file));
        for(unsigned i=0;i<30;i++){
            char name[32];snprintf(name,sizeof(name),"batch-%u",i);efs_ino_t extra;
            CHECK(!efs_meta_apply_create_file(kv,&attrs,EFS_ROOT_INO,S_IFREG|0644,name,&extra));
            struct efs_meta_row row;uint8_t k[EFS_KV_KEY_MAX],v[EFS_META_INO_BYTES];uint32_t kn;
            CHECK(!efs_meta_apply_get_inode(kv,extra,&row));
            row.base_size=i+1;row.base_mtime=200+i;
            CHECK(!efs_kv_key_inode(efs_kv_inode_shard(extra),extra,k,&kn));
            CHECK(!efs_meta_pack_inode(&row,v,sizeof(v)));CHECK(!efs_kv_put(kv,k,kn,v,sizeof(v)));
        }
        CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));raw_syncs=index_syncs=0;
        CHECK(!efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,&old));
        if(old){
            struct efs_query_rollup totals;
            CHECK(!efs_query_disk_stats(old,EFS_ROOT_INO,&totals));
            CHECK(totals.imm_files==31 && totals.tree_files==31 && totals.imm_bytes==465 && totals.tree_bytes==465);
            CHECK(totals.imm_tmin==100 && totals.imm_tmax==229);
            unsigned populated=0;for(unsigned i=0;i<SHARDS;i++)populated+=old->part[i].count!=0;
            CHECK(raw_syncs==populated && index_syncs==populated);
            uint64_t wm[4]={1,2,3,4};CHECK(!efs_query_disk_checkpoint(old,wm));
            read_calls=0;
            CHECK(!efs_query_disk_build(cache,source,old,4<<20,NULL,NULL,&next));
            CHECK(read_calls==0); /* reuse performs no inode/dentry discovery */
            efs_query_disk_free(next);next=NULL;
            uint64_t saved_total=aggregate_limit;aggregate_limit=1;
            CHECK(efs_query_disk_build(cache,source,old,4<<20,NULL,NULL,&next)==EFS_ERR_QUOTA && !next);
            CHECK(!efs_query_disk_restore(cache,wm,4<<20,&next));
            CHECK(!efs_query_disk_inode(next,file,NULL,NULL));efs_query_disk_free(next);next=NULL;
            aggregate_limit=saved_total;
            /* A short derived-row batch followed by ENOSPC must never
             * publish a partially materialized candidate or replace old. */
            fail_write_kind='d';write_faults=0;
            CHECK(efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,&next)==EFS_ERR_IO && !next);
            CHECK(write_faults==1 && !fail_write_kind);
            CHECK(!efs_query_disk_restore(cache,wm,4<<20,&next));
            CHECK(!efs_query_disk_inode(next,file,NULL,NULL));efs_query_disk_free(next);next=NULL;
            /* Fail the first coalesced parent write after all inode batches.
             * Candidate-only accumulated totals must not escape on failure. */
            unsigned batches=0;
            for(unsigned i=0;i<SHARDS;i++)batches+=(old->part[i].inodes+NODE_BATCH-1)/NODE_BATCH;
            fail_derived_after=batches+1;write_faults=0;
            CHECK(efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,&next)==EFS_ERR_IO && !next);
            CHECK(write_faults==1 && !fail_derived_after);
            CHECK(!efs_query_disk_restore(cache,wm,4<<20,&next));
            CHECK(!efs_query_disk_inode(next,file,NULL,NULL));efs_query_disk_free(next);next=NULL;
            const char kinds_to_fail[]={'r','i','n','d','a','m'};
            for(unsigned i=0;i<sizeof(kinds_to_fail);i++){
                fail_sync_kind=kinds_to_fail[i];sync_faults=0;
                CHECK(efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,&next)==EFS_ERR_IO && !next);
                CHECK(sync_faults==1);fail_sync_kind=0;
                CHECK(!efs_query_disk_inode(old,file,NULL,NULL));
                CHECK(!efs_query_disk_restore(cache,wm,4<<20,&next));
                CHECK(!efs_query_disk_inode(next,file,NULL,NULL));efs_query_disk_free(next);next=NULL;
            }
        }
        if(old){
            struct efs_meta_row row;uint8_t k[EFS_KV_KEY_MAX],v[EFS_META_INO_BYTES];uint32_t kn;
            CHECK(!efs_meta_apply_get_inode(kv,file,&row));row.base_size=999;row.base_mtime=999;
            CHECK(!efs_kv_key_inode(efs_kv_inode_shard(file),file,k,&kn));CHECK(!efs_meta_pack_inode(&row,v,sizeof(v)));CHECK(!efs_kv_put(kv,k,kn,v,sizeof(v)));
            struct efs_kv_lsm_snapshot *changed=NULL;CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&changed));
            fail_write_kind='d';write_faults=0;
            CHECK(efs_query_disk_build(cache,changed,old,4<<20,NULL,NULL,&next)==EFS_ERR_IO && !next);
            CHECK(write_faults==1 && !fail_write_kind);
            struct efs_query_rollup totals;CHECK(!efs_query_disk_stats(old,EFS_ROOT_INO,&totals));CHECK(totals.tree_bytes==465);
            uint64_t wm[4]={1,2,3,4};CHECK(!efs_query_disk_restore(cache,wm,4<<20,&next));
            CHECK(!efs_query_disk_stats(next,EFS_ROOT_INO,&totals));CHECK(totals.tree_bytes==465);
            efs_query_disk_free(next);next=NULL;efs_kv_lsm_snapshot_release(changed);
        }
        efs_query_disk_free(old);efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
    }
    printf("query disk I/O: windows, lookup, packed records, partial ENOSPC, FD reuse and fsync refusal %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
