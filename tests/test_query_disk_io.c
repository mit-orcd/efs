/* Exercise the production cache boundary and partial-write error behavior. */
#include "efs/dir_layout.h"
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
static unsigned alias_opens;
static unsigned raw_syncs,index_syncs,sync_faults;
static char fail_sync_kind,fail_write_kind;
static unsigned write_faults,fail_derived_after;
static char descriptor_kind[4096];
static unsigned char writing_raw[4096];
static unsigned raw_handles,raw_handles_high;
#ifdef __linux__
static int writeback_error;
static unsigned writeback_calls;
static int fault_writeback(int fd,off64_t offset,off64_t bytes,unsigned flags)
{
    writeback_calls++;
    if(writeback_error){errno=writeback_error;return -1;}
    return sync_file_range(fd,offset,bytes,flags);
}
#endif
static int tracked_open(const char *path,int flags,...)
{
    mode_t mode=0;
    if(flags&O_CREAT){va_list args;va_start(args,flags);mode=va_arg(args,int);va_end(args);}
    int fd=open(path,flags,mode);
    if(fd>=0 && fd<(int)sizeof(descriptor_kind)){
        /* Test code also closes descriptors outside the production wrapper. */
        if(writing_raw[fd]){writing_raw[fd]=0;raw_handles--;}
        size_t n=strlen(path);char kind=0;
        if(!(flags&O_DIRECTORY)){
            if(n>=2 && path[n-2]=='.')kind=path[n-1];
            const char *name=strrchr(path,'/');if(name && !strcmp(name+1,"manifest"))kind='m';
        }
        descriptor_kind[fd]=kind;
        if(kind=='a')alias_opens++;
        if((flags&O_CREAT) && (kind=='r' || kind=='i')){
            writing_raw[fd]=1;raw_handles++;
            if(raw_handles>raw_handles_high)raw_handles_high=raw_handles;
        }
    }
    return fd;
}
static int tracked_close(int fd)
{
    if(fd>=0 && fd<(int)sizeof(writing_raw)){
        if(writing_raw[fd]){writing_raw[fd]=0;raw_handles--;}
        descriptor_kind[fd]=0;
    }
    return close(fd);
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
#define close tracked_close
#ifdef __linux__
#define sync_file_range fault_writeback
#endif
#include "../src/meta/query_disk.c"
#undef pwrite
#undef pread
#undef fsync
#undef open
#undef close
#ifdef __linux__
#undef sync_file_range
#endif
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
    CHECK(!efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&disk));
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
static void raw_sync_batch_test(void)
{
    char root[]="/tmp/efs-query-sync-batch-XXXXXX";CHECK(mkdtemp(root));
    struct efs_query_disk *d=calloc(1,sizeof(*d));CHECK(d);if(!d)return;
    unsigned modes=3;
#ifdef __linux__
    modes=5;
#endif
    for(unsigned mode=0;mode<modes;mode++){
        struct raw_sync_batch batch={0};int descriptors[3];char paths[3][256];
        memset(writing_raw,0,sizeof(writing_raw));raw_handles=raw_handles_high=0;
        for(unsigned i=0;i<3;i++){
            snprintf(paths[i],sizeof(paths[i]),"%s/%u-%u.r",root,mode,i);
            descriptors[i]=tracked_open(paths[i],O_CREAT|O_EXCL|O_RDWR,0600);
            CHECK(descriptors[i]>=0);if(descriptors[i]>=0){
                CHECK(pwrite(descriptors[i],"data",4,0)==4);
                batch.fd[batch.count++]=descriptors[i];
            }
        }
        unsigned before=raw_syncs;sync_faults=0;
        d->stop=mode==1?cancel_budget:NULL;
        if(mode==2)fail_sync_kind='r';
#ifdef __linux__
        writeback_calls=0;
        writeback_error=mode==3?ENOSYS:mode==4?EIO:0;
#endif
        int rc=raw_sync_finish(d,&batch,EFS_OK);
        CHECK(rc==(mode==0 || mode==3?EFS_OK:mode==1?EFS_ERR_BUSY:EFS_ERR_IO));
        CHECK(raw_syncs-before==(mode==0 || mode==3?3:mode==2?1:0));
        CHECK(sync_faults==(mode==2));fail_sync_kind=0;
#ifdef __linux__
        CHECK(writeback_calls==(mode==1?0:mode==4?1:3));
        writeback_error=0;
#endif
        CHECK(batch.count==0 && raw_handles==0);
        for(unsigned i=0;i<3;i++){
            errno=0;CHECK(fcntl(descriptors[i],F_GETFD)==-1 && errno==EBADF);
            CHECK(!unlink(paths[i]));
        }
    }
    free(d);CHECK(!rmdir(root));
}
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
static void merge_test(void)
{
    char root[]="/tmp/efs-query-merge-XXXXXX";CHECK(mkdtemp(root));
    struct efs_query_disk *d=calloc(1,sizeof(*d));CHECK(d);if(!d)return;
    for(unsigned kind=0;kind<2;kind++) {
        size_t width=kind?sizeof(struct disk_alias):sizeof(struct efs_name_entry);
        int (*compare)(const void *,const void *)=kind?alias_compare:name_compare;
        const unsigned counts[2]={2001,4097},total=6098;
        uint8_t *expected=calloc(total,width),*actual=calloc(total,width);
        uint8_t *scratch=malloc(SORT_ROWS*width);CHECK(expected && actual && scratch);
        int fds[3];char path[EFS_MAX_PATH];
        for(unsigned j=0;j<3;j++) {
            snprintf(path,sizeof(path),"%s/%u-%u",root,kind,j);
            fds[j]=open(path,O_CREAT|O_RDWR,0600);CHECK(fds[j]>=0);unlink(path);
        }
        unsigned at=0;
        for(unsigned j=0;j<2;j++)for(unsigned n=0;n<counts[j];n++,at++) {
            if(kind) {
                struct disk_alias e={2*n+j,17,at};memcpy(expected+at*width,&e,width);
            } else {
                struct efs_name_entry e={0};e.parent=17;
                snprintf(e.name,sizeof(e.name),"item-%08u",2*n+j);
                memcpy(expected+at*width,&e,width);
            }
            CHECK(pwrite(fds[j],expected+at*width,width,n*width)==(ssize_t)width);
        }
        qsort(expected,total,width,compare);read_calls=write_calls=0;
        CHECK(!merge_names(d,fds[0],fds[1],fds[2],counts[0],counts[1],width,compare,scratch));
        CHECK(read_calls<20 && write_calls<12);
        printf("merge %s: rows=%u reads=%u writes=%u\n",kind?"aliases":"names",total,read_calls,write_calls);
        CHECK(pread(fds[2],actual,total*width,0)==(ssize_t)(total*width));
        CHECK(!memcmp(expected,actual,total*width));
        /* A partial output followed by ENOSPC must propagate, never success. */
        CHECK(!ftruncate(fds[2],0));partial_write=1;
        CHECK(merge_names(d,fds[0],fds[1],fds[2],counts[0],counts[1],width,compare,scratch)==EFS_ERR_IO);
        CHECK(!partial_write);
        d->stop=cancel_budget;read_calls=write_calls=0;
        CHECK(merge_names(d,fds[0],fds[1],fds[2],counts[0],counts[1],width,compare,scratch)==EFS_ERR_BUSY);
        CHECK(!read_calls && !write_calls);d->stop=NULL;
        for(unsigned j=0;j<3;j++)close(fds[j]);
        free(scratch);free(actual);free(expected);
    }
    free(d);CHECK(!rmdir(root));
}
static void alias_batch_test(void)
{
    char root[]="/tmp/efs-query-alias-batch-XXXXXX";CHECK(mkdtemp(root));
    struct efs_query_disk *d=calloc(1,sizeof(*d));CHECK(d);if(!d)return;
    d->budget_fd=-1;d->disk_limit=UINT64_MAX;strcpy(d->path,root);
    pthread_mutex_init(&d->mutex,NULL);
    for(unsigned i=0;i<FD_CACHE;i++)d->fds[i].fd=-1;
    const unsigned shards=FD_CACHE*2;
    struct disk_alias alias={UINT64_MAX,EFS_ROOT_INO,0};
    for(unsigned i=0;i<shards;i++){
        d->part[i].names=1;int fd=make_file(d,i,'a');CHECK(fd>=0);
        if(fd>=0){CHECK(!io_at(fd,&alias,sizeof(alias),0,1));close(fd);}
    }
    struct incremental state={.changes_fd=-1};efs_ino_t ids[CHANGE_BATCH];
    for(unsigned i=0;i<CHANGE_BATCH;i++)ids[i]=100+i;
    alias_opens=read_calls=0;
    CHECK(!aliases_dirty_batch(d,&state,ids,CHANGE_BATCH));
    unsigned batch_opens=alias_opens,batch_reads=read_calls;
    CHECK(batch_opens==shards && batch_reads==2*shards && !state.count);
    close_cache(d);alias_opens=read_calls=0;
    /* Proven endpoints eliminate repeated opens even across separate batches. */
    for(unsigned i=0;i<CHANGE_BATCH;i++)CHECK(!aliases_dirty_batch(d,&state,ids+i,1));
    CHECK(!alias_opens && !read_calls);
    /* Fresh endpoint state models the uncached traversal between IDs. */
    for(unsigned i=0;i<CHANGE_BATCH;i++){
        memset(state.alias_known,0,sizeof(state.alias_known));
        CHECK(!aliases_dirty_batch(d,&state,ids+i,1));
    }
    CHECK(alias_opens==shards*CHANGE_BATCH && read_calls==2*alias_opens);
    printf("alias batch: shards=%u ids=%u opens=%u->%u reads=%u->%u\n",shards,CHANGE_BATCH,alias_opens,batch_opens,read_calls,batch_reads);
    close_cache(d);d->stop=cancel_budget;alias_opens=0;
    CHECK(aliases_dirty_batch(d,&state,ids,CHANGE_BATCH)==EFS_ERR_BUSY && !alias_opens);
    d->stop=NULL;char path[EFS_MAX_PATH];
    CHECK(!filename(d,FD_CACHE/2,'a',path,sizeof(path)));
    CHECK(!truncate(path,sizeof(alias)-1));
    memset(state.alias_known,0,sizeof(state.alias_known));
    CHECK(aliases_dirty_batch(d,&state,ids,CHANGE_BATCH)==EFS_ERR_IO);
    close_cache(d);
    for(unsigned i=0;i<shards;i++){
        CHECK(!filename(d,i,'a',path,sizeof(path)));CHECK(!unlink(path));
    }
    pthread_mutex_destroy(&d->mutex);free(d);CHECK(!rmdir(root));
}

static void changed_journal_test(void)
{
    const unsigned count=2*SORT_ROWS+17,shards=FD_CACHE*2;
    for(unsigned mode=0;mode<5;mode++){
        char root[]="/tmp/efs-query-changes-XXXXXX";CHECK(mkdtemp(root));
        struct efs_query_disk *d=calloc(1,sizeof(*d));struct incremental *state=calloc(1,sizeof(*state));
        CHECK(d && state);if(!d || !state){free(d);free(state);continue;}
        d->budget_fd=-1;d->disk_limit=UINT64_MAX;strcpy(d->path,root);pthread_mutex_init(&d->mutex,NULL);
        for(unsigned i=0;i<FD_CACHE;i++)d->fds[i].fd=-1;
        state->changes=count;state->change_bytes=count*sizeof(efs_ino_t);state->changes_fd=make_file(d,0,'c');
        CHECK(state->changes_fd>=0);CHECK(!charge_disk(d,state->change_bytes));
        for(unsigned at=0;at<count;at+=SORT_ROWS){
            unsigned n=count-at>SORT_ROWS?SORT_ROWS:count-at;
            for(unsigned i=0;i<n;i++)state->sort_ids[i]=count-at-i;
            CHECK(!io_at(state->changes_fd,state->sort_ids,n*sizeof(efs_ino_t),at*sizeof(efs_ino_t),1));
        }
        uint64_t before=d->disk_bytes;
        if(mode==1)d->disk_limit=before+2*state->change_bytes-1;
        if(mode==2)partial_write=1;
        if(mode==3)d->stop=cancel_budget;
        if(mode==4)CHECK(!ftruncate(state->changes_fd,state->change_bytes-1));
        int rc=changes_sort(d,state);
        CHECK(rc==(mode==1?EFS_ERR_QUOTA:mode==3?EFS_ERR_BUSY:mode?EFS_ERR_IO:EFS_OK));
        CHECK(!partial_write);d->stop=NULL;d->disk_limit=UINT64_MAX;
        if(!mode){
            CHECK(d->disk_bytes==before);
            for(unsigned at=0;at<count;at+=SORT_ROWS){
                unsigned n=count-at>SORT_ROWS?SORT_ROWS:count-at;
                CHECK(!io_at(state->changes_fd,state->sort_ids,n*sizeof(efs_ino_t),at*sizeof(efs_ino_t),0));
                for(unsigned i=0;i<n;i++)CHECK(state->sort_ids[i]==at+i+1);
            }
            struct disk_alias alias={UINT64_MAX,EFS_ROOT_INO,0};
            for(unsigned i=0;i<shards;i++){
                d->part[i].names=1;int fd=make_file(d,i,'a');CHECK(fd>=0);
                if(fd>=0){CHECK(!io_at(fd,&alias,sizeof(alias),0,1));close(fd);}
            }
            alias_opens=0;CHECK(!aliases_dirty_sorted(d,state));CHECK(alias_opens==shards && !state->count);
            close_cache(d);d->stop=cancel_budget;alias_opens=0;
            CHECK(aliases_dirty_sorted(d,state)==EFS_ERR_BUSY && !alias_opens);d->stop=NULL;
            char path[EFS_MAX_PATH];CHECK(!filename(d,0,'a',path,sizeof(path)));CHECK(!truncate(path,sizeof(alias)-1));
            CHECK(aliases_dirty_sorted(d,state)==EFS_ERR_IO);
        }else{
            efs_ino_t first=0;CHECK(!io_at(state->changes_fd,&first,sizeof(first),0,0));CHECK(first==count);
        }
        close(state->changes_fd);free(state);efs_query_disk_free(d);CHECK(access(root,F_OK)<0);
    }
    puts("changed journal: external sort, one-pass aliases, charged peak, partial writes/EOF and cancellation PASS");
}

static void pending_preflight_test(void)
{
    char store[]="/tmp/efs-query-pending-kv-XXXXXX",cache[]="/tmp/efs-query-pending-cache-XXXXXX";
    CHECK(mkdtemp(store));CHECK(mkdtemp(cache));
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);CHECK(kv);if(!kv)return;
    struct efs_meta_attrs attrs={1000,1000,100};CHECK(!efs_meta_apply_init(kv,100));
    for(unsigned i=0;i<32;i++){
        char name[32];efs_ino_t ino;snprintf(name,sizeof(name),"ordinary-%u",i);
        CHECK(!efs_meta_apply_create_file(kv,&attrs,EFS_ROOT_INO,S_IFREG|0644,name,&ino));
    }
    struct efs_meta_row row={.ino=SHARDS-1,.parent=EFS_ROOT_INO,.generation=1,.mode=S_IFREG|0644};
    uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_INO_BYTES];uint32_t kn;
    CHECK(!efs_kv_key_inode(SHARDS-1,row.ino,key,&kn));CHECK(!efs_meta_pack_inode(&row,value,sizeof(value)));
    CHECK(!efs_kv_put(kv,key,kn,value,sizeof(value)));
    struct efs_kv_lsm_snapshot *base=NULL,*blocked=NULL,*clean=NULL;struct efs_query_disk *old=NULL,*next=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&base));CHECK(!efs_query_disk_build(cache,base,NULL,4<<20,NULL,NULL,NULL,&old));
    struct efs_txid tx={{0}};tx.bytes[15]=67;struct efs_txn_parts parts={.n=1,.shard={SHARDS-1}};
    uint64_t version;CHECK(!efs_txn_ver_get(kv,key,kn,&version));CHECK(!efs_txn_prepare_guard(kv,&tx,&parts,key,kn,version));
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&blocked));
    struct efs_query_build_failure failure={0};struct efs_query_build_times times;
    unsigned writes=write_calls,syncs=raw_syncs+index_syncs;
    CHECK(efs_query_disk_build_checked(cache,blocked,old,4<<20,NULL,NULL,&times,&failure,&next)==EFS_ERR_BUSY);
    CHECK(failure.pending && !next && write_calls==writes && raw_syncs+index_syncs==syncs);
    CHECK(efs_query_disk_build_checked(cache,blocked,old,4<<20,cancel_budget,NULL,&times,&failure,&next)==EFS_ERR_BUSY);
    CHECK(!failure.pending && !next && write_calls==writes);
    CHECK(!efs_txn_resolve(kv,&tx,SHARDS-1,EFS_TXN_ABORT));CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&clean));
    CHECK(!efs_query_disk_build_checked(cache,clean,old,4<<20,NULL,NULL,&times,&failure,&next));CHECK(!failure.pending && next);
    efs_query_disk_free(next);next=NULL;writes=write_calls;
    CHECK(efs_query_disk_build_checked(cache,blocked,old,4<<20,NULL,NULL,&times,&failure,&next)==EFS_ERR_BUSY);
    CHECK(failure.pending && !next && write_calls==writes);
    CHECK(!efs_query_disk_inode(old,row.ino,NULL,NULL));
    efs_query_disk_free(old);efs_kv_lsm_snapshot_release(base);efs_kv_lsm_snapshot_release(blocked);efs_kv_lsm_snapshot_release(clean);efs_kv_lsm_close(kv);
    puts("pending preflight: high-shard guard refuses before writes, shutdown distinguished, immutable cut and fresh resolved rebuild PASS");
}

static void orphan_preflight_test(void)
{
    char store[]="/tmp/efs-query-orphan-kv-XXXXXX",cache[]="/tmp/efs-query-orphan-cache-XXXXXX";
    CHECK(mkdtemp(store));CHECK(mkdtemp(cache));
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};
    struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);CHECK(kv);if(!kv)return;
    struct efs_meta_attrs attrs={1000,1000,100};efs_ino_t parent,child;
    CHECK(!efs_meta_apply_init(kv,100));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"parent",&parent));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,parent,S_IFDIR|0755,"child",&child));
    uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_DENT_BYTES];uint32_t kn,vn=sizeof(value);
    CHECK(!efs_kv_key_dentry(efs_kv_inode_shard(parent),parent,"child",key,&kn));
    CHECK(!efs_kv_get(kv,key,kn,value,&vn));CHECK(!efs_kv_del(kv,key,kn));
    struct efs_kv_lsm_snapshot *source=NULL;struct efs_query_disk *disk=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    struct efs_query_build_failure failure={0};struct efs_query_build_times times;
    CHECK(efs_query_disk_build_checked(cache,source,NULL,4<<20,NULL,NULL,&times,&failure,&disk)==EFS_ERR_IO);
    CHECK(!disk && failure.directory==child);
    unsigned writes=write_calls,syncs=raw_syncs+index_syncs;
    CHECK(efs_query_disk_build_checked(cache,source,NULL,4<<20,NULL,NULL,&times,&failure,&disk)==EFS_ERR_IO);
    CHECK(!disk && failure.directory==child && write_calls==writes && raw_syncs+index_syncs==syncs);
    CHECK(orphan_preflight(source,&failure,cancel_budget,NULL)==EFS_ERR_BUSY && failure.directory==child);
    uint8_t other_key[EFS_KV_KEY_MAX];uint32_t other_kn;
    CHECK(!efs_kv_key_dentry(efs_kv_inode_shard(EFS_ROOT_INO),EFS_ROOT_INO,"moved",other_key,&other_kn));
    CHECK(!efs_kv_put(kv,other_key,other_kn,value,vn));
    struct efs_kv_lsm_snapshot *moved=NULL;CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&moved));
    CHECK(!orphan_preflight(moved,&failure,NULL,NULL) && !failure.directory);
    efs_kv_lsm_snapshot_release(moved);failure.directory=child;
    CHECK(!efs_kv_del(kv,other_key,other_kn));
    CHECK(!efs_kv_put(kv,key,kn,value,vn));
    /* A later live repair cannot change the old immutable source's verdict. */
    CHECK(orphan_preflight(source,&failure,NULL,NULL)==EFS_ERR_IO);
    efs_kv_lsm_snapshot_release(source);source=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!orphan_preflight(source,&failure,NULL,NULL) && !failure.directory);
    CHECK(!efs_query_disk_build_checked(cache,source,NULL,4<<20,NULL,NULL,&times,&failure,&disk));
    CHECK(disk && !failure.directory);efs_query_disk_free(disk);disk=NULL;
    efs_kv_lsm_snapshot_release(source);source=NULL;
    CHECK(!efs_meta_dir_begin_split(kv,parent));
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));failure.directory=child;
    CHECK(!orphan_preflight(source,&failure,NULL,NULL) && !failure.directory);
    efs_kv_lsm_snapshot_release(source);source=NULL;
    while(efs_meta_dir_migrate_one(kv,parent)==EFS_OK){}
    CHECK(!efs_meta_dir_finish_hashed(kv,parent));
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));failure.directory=child;
    CHECK(!orphan_preflight(source,&failure,NULL,NULL) && !failure.directory);
    efs_kv_lsm_snapshot_release(source);source=NULL;
    CHECK(!efs_kv_key_dentry(efs_kv_dentry_shard(parent,"child",EFS_META_LAYOUT_HASHED),parent,"child",key,&kn));
    CHECK(!efs_kv_del(kv,key,kn));
    uint8_t parent_key[EFS_KV_KEY_MAX];uint32_t parent_kn;
    CHECK(!efs_kv_key_inode(efs_kv_inode_shard(parent),parent,parent_key,&parent_kn));
    struct efs_txid tx={{0}};tx.bytes[15]=22;
    struct efs_txn_parts parts={.n=1,.shard={efs_kv_inode_shard(parent)}};
    uint64_t parent_ver;CHECK(!efs_txn_ver_get(kv,parent_key,parent_kn,&parent_ver));
    CHECK(!efs_txn_prepare_guard(kv,&tx,&parts,parent_key,parent_kn,parent_ver));
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));failure.directory=child;
    CHECK(orphan_preflight(source,&failure,NULL,NULL)==EFS_ERR_BUSY && failure.directory==child);
    efs_kv_lsm_snapshot_release(source);source=NULL;
    CHECK(!efs_txn_resolve(kv,&tx,parts.shard[0],EFS_TXN_ABORT));
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));failure.directory=UINT64_MAX;
    CHECK(!orphan_preflight(source,&failure,NULL,NULL) && !failure.directory);
    efs_kv_lsm_snapshot_release(source);source=NULL;
    /* The preflight budget is not a directory size limit: inconclusive wide
     * scans must fall back to the original complete builder. */
    for(unsigned i=0;i<4100;i++){
        char name[32];efs_ino_t ino;snprintf(name,sizeof(name),"wide-%04u",i);
        CHECK(!efs_meta_apply_create_file(kv,&attrs,parent,S_IFREG|0644,name,&ino));
    }
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));failure.directory=child;
    CHECK(!orphan_preflight(source,&failure,NULL,NULL) && !failure.directory);
    efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
    puts("orphan preflight: actual failed graph, no repeated scratch writes/syncs, immutable repair, prepared refusal and fresh rebuild PASS");
}

int main(void)
{
    raw_sync_batch_test();
    orphan_preflight_test();
    alias_batch_test();
    changed_journal_test();
    pending_preflight_test();
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
    merge_test();
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
    CHECK(!locate(d,17*8192,&found,&at) && at==16);
    CHECK(locate(d,17*8192+4096,&found,NULL)==EFS_ERR_NOT_FOUND);
    CHECK(!locate(d,2*8192,&found,&at) && at==1);
    CHECK(!locate(d,16*8192,&found,&at) && at==15);
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
    search.buffer=malloc(2*RAW_WRITE_WINDOW);CHECK(search.buffer);
    d->disk_limit=UINT64_MAX;
    uint8_t search_key[11]={0,98,EFS_KV_KIND_INODE},search_value=0;
    for(unsigned i=0;i<31;i++){
        search_key[10]=(uint8_t)(2*i+2);search_value=(uint8_t)i;
        CHECK(!save(&search,search_key,sizeof(search_key),&search_value,1));
    }
    CHECK(!writer_flush(&search));close(search.raw);close(search.index);free(search.buffer);
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
    /* Raw batches preserve bytes, offsets, empty values, and flush the raw
     * data before any matching index slot. Failure must not publish a view. */
    uint8_t key[11]={0,99,EFS_KV_KIND_INODE},value[37],record[48];
    memset(value,0x39,sizeof(value));d->disk_limit=UINT64_MAX;
    struct writer writer={.d=d,.shard=99,.raw=-1,.index=-1};
    writer.buffer=malloc(2*RAW_WRITE_WINDOW);CHECK(writer.buffer);
    write_calls=0;CHECK(!save(&writer,key,sizeof(key),value,sizeof(value)));
    CHECK(!write_calls && writer.count==1 && writer.offset==sizeof(record));
    CHECK(!writer_flush(&writer));CHECK(write_calls==2);
    CHECK(!io_at(writer.raw,record,sizeof(record),0,0));
    CHECK(!memcmp(record,key,sizeof(key)) && !memcmp(record+sizeof(key),value,sizeof(value)));
    CHECK(!save(&writer,key,sizeof(key),NULL,0));CHECK(writer.count==2);
    CHECK(!writer_flush(&writer));
    struct slot slot;CHECK(!io_at(writer.index,&slot,sizeof(slot),sizeof(slot),0));
    CHECK(slot.offset==sizeof(record) && !slot.value_len && slot.key_len==sizeof(key));
    CHECK(!save(&writer,key,sizeof(key),value,sizeof(value)));
    partial_write=1;CHECK(writer_flush(&writer)==EFS_ERR_IO);
    struct stat index_stat;CHECK(!fstat(writer.index,&index_stat));
    CHECK(index_stat.st_size==(off_t)(2*sizeof(struct slot)));
    close(writer.raw);close(writer.index);free(writer.buffer);
    struct writer batch={.d=d,.shard=97,.raw=-1,.index=-1};
    batch.buffer=malloc(2*RAW_WRITE_WINDOW);CHECK(batch.buffer);
    key[1]=97;write_calls=0;
    for(unsigned i=0;i<10000;i++) {
        key[9]=(uint8_t)(i>>8);key[10]=(uint8_t)i;
        CHECK(!save(&batch,key,sizeof(key),value,sizeof(value)));
    }
    CHECK(!writer_flush(&batch));CHECK(write_calls<20);
    printf("raw batch: records=10000 writes=%u\n",write_calls);
    for(unsigned i=0;i<10000;i++) {
        CHECK(!io_at(batch.index,&slot,sizeof(slot),i*sizeof(slot),0));
        CHECK(slot.offset==(uint64_t)i*sizeof(record) && slot.key_len==sizeof(key) && slot.value_len==sizeof(value));
        CHECK(!io_at(batch.raw,record,sizeof(record),slot.offset,0));
        CHECK(record[9]==(uint8_t)(i>>8) && record[10]==(uint8_t)i && !memcmp(record+sizeof(key),value,sizeof(value)));
    }
    close(batch.raw);close(batch.index);free(batch.buffer);
    struct writer empty={.d=d,.shard=96,.raw=-1,.index=-1};
    empty.buffer=malloc(2*RAW_WRITE_WINDOW);CHECK(empty.buffer);key[1]=96;write_calls=0;
    for(unsigned i=0;i<10000;i++)CHECK(!save(&empty,key,sizeof(key),NULL,0));
    CHECK(!writer_flush(&empty));CHECK(write_calls==6); /* Index window fills first. */
    uint8_t *maximum=malloc(VALUE_MAX);CHECK(maximum);memset(maximum,0xa7,VALUE_MAX);
    uint64_t before=empty.offset;
    for(unsigned i=0;i<20;i++)CHECK(!save(&empty,key,sizeof(key),maximum,VALUE_MAX));
    CHECK(!writer_flush(&empty));
    for(unsigned i=0;i<20;i++) {
        CHECK(!io_at(empty.index,&slot,sizeof(slot),(10000+i)*sizeof(slot),0));
        CHECK(slot.offset==before+(uint64_t)i*(sizeof(key)+VALUE_MAX) && slot.value_len==VALUE_MAX);
        uint8_t tail;CHECK(!io_at(empty.raw,&tail,1,slot.offset+sizeof(key)+VALUE_MAX-1,0));CHECK(tail==0xa7);
    }
    d->stop=cancel_budget;CHECK(save(&empty,key,sizeof(key),NULL,0)==EFS_ERR_BUSY);d->stop=NULL;
    CHECK(!save(&empty,key,sizeof(key),NULL,0));fail_write_kind='i';write_faults=0;
    CHECK(writer_flush(&empty)==EFS_ERR_IO && write_faults==1 && !fail_write_kind);
    CHECK(!fstat(empty.index,&index_stat));CHECK(index_stat.st_size==(off_t)(10020*sizeof(slot)+3));
    close(empty.raw);close(empty.index);free(empty.buffer);free(maximum);
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
        /* Inactive historical lanes remain valid snapshot inputs. Spread
         * them across 64 physical raw partitions so this production build
         * exercises full sync batches and a nonempty final tail. */
        struct efs_meta_row lane_owner;CHECK(!efs_meta_apply_get_inode(kv,file,&lane_owner));
        for(unsigned lane=0;lane<64;lane++){
            uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_LANE_BYTES]={0};uint32_t kn;
            CHECK(!efs_kv_key_lane(efs_kv_lane_shard(file,(uint8_t)lane),file,
                                  lane_owner.generation,(uint8_t)lane,key,&kn));
            CHECK(!efs_kv_put(kv,key,kn,value,sizeof(value)));
        }
        CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));raw_syncs=index_syncs=0;
        memset(writing_raw,0,sizeof(writing_raw));raw_handles=raw_handles_high=0;
        CHECK(!efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&old));
        CHECK(raw_handles==0 && raw_handles_high==RAW_SYNC_FILES);
        printf("raw sync batch: writing_handles_high=%u bound=%u retained=0\n",raw_handles_high,RAW_SYNC_FILES);
        if(old){
            struct efs_query_rollup totals;
            CHECK(!efs_query_disk_stats(old,EFS_ROOT_INO,&totals));
            CHECK(totals.imm_files==31 && totals.tree_files==31 && totals.imm_bytes==465 && totals.tree_bytes==465);
            CHECK(totals.imm_tmin==100 && totals.imm_tmax==229);
            unsigned populated=0;for(unsigned i=0;i<SHARDS;i++)populated+=old->part[i].count!=0;
            CHECK(raw_syncs==populated && index_syncs==populated);
            uint64_t wm[4]={1,2,3,4};CHECK(!efs_query_disk_checkpoint(old,wm));
            read_calls=0;
            CHECK(!efs_query_disk_build(cache,source,old,4<<20,NULL,NULL,NULL,&next));
            CHECK(read_calls==0); /* reuse performs no inode/dentry discovery */
            efs_query_disk_free(next);next=NULL;
            uint64_t saved_total=aggregate_limit;aggregate_limit=1;
            CHECK(efs_query_disk_build(cache,source,old,4<<20,NULL,NULL,NULL,&next)==EFS_ERR_QUOTA && !next);
            CHECK(!efs_query_disk_restore(cache,wm,4<<20,&next));
            CHECK(!efs_query_disk_inode(next,file,NULL,NULL));efs_query_disk_free(next);next=NULL;
            aggregate_limit=saved_total;
            /* A short derived-row batch followed by ENOSPC must never
             * publish a partially materialized candidate or replace old. */
            fail_write_kind='d';write_faults=0;
            CHECK(efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&next)==EFS_ERR_IO && !next);
            CHECK(write_faults==1 && !fail_write_kind);
            CHECK(!efs_query_disk_restore(cache,wm,4<<20,&next));
            CHECK(!efs_query_disk_inode(next,file,NULL,NULL));efs_query_disk_free(next);next=NULL;
            /* Fail the first coalesced parent write after all inode batches.
             * Candidate-only accumulated totals must not escape on failure. */
            unsigned batches=0;
            for(unsigned i=0;i<SHARDS;i++)batches+=(old->part[i].inodes+NODE_BATCH-1)/NODE_BATCH;
            fail_derived_after=batches+1;write_faults=0;
            CHECK(efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&next)==EFS_ERR_IO && !next);
            CHECK(write_faults==1 && !fail_derived_after);
            CHECK(!efs_query_disk_restore(cache,wm,4<<20,&next));
            CHECK(!efs_query_disk_inode(next,file,NULL,NULL));efs_query_disk_free(next);next=NULL;
            const char kinds_to_fail[]={'r','i','n','d','a','m'};
#ifdef __linux__
            /* A scheduling-hint I/O error is also an unpublished candidate,
             * never a full-rebuild request or a lost writing descriptor. */
            writeback_error=EIO;writeback_calls=0;
            CHECK(efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&next)==EFS_ERR_IO && !next);
            CHECK(writeback_calls==1 && raw_handles==0);writeback_error=0;
            CHECK(!efs_query_disk_restore(cache,wm,4<<20,&next));
            CHECK(!efs_query_disk_inode(next,file,NULL,NULL));efs_query_disk_free(next);next=NULL;
#endif
            for(unsigned i=0;i<sizeof(kinds_to_fail);i++){
                fail_sync_kind=kinds_to_fail[i];sync_faults=0;
                CHECK(efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&next)==EFS_ERR_IO && !next);
                CHECK(raw_handles==0 && raw_handles_high<=RAW_SYNC_FILES);
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
            /* A partial change-journal write must abort the incremental
             * candidate, preserve the retained view and never trigger FULL. */
            fail_write_kind='c';write_faults=0;
            CHECK(efs_query_disk_build(cache,changed,old,4<<20,NULL,NULL,NULL,&next)==EFS_ERR_IO && !next);
            CHECK(write_faults==1 && !fail_write_kind);
            struct efs_query_rollup retained;CHECK(!efs_query_disk_stats(old,EFS_ROOT_INO,&retained));CHECK(retained.tree_bytes==465);
            fail_write_kind='d';write_faults=0;
            CHECK(efs_query_disk_build(cache,changed,old,4<<20,NULL,NULL,NULL,&next)==EFS_ERR_IO && !next);
            CHECK(write_faults==1 && !fail_write_kind);
            struct efs_query_rollup totals;CHECK(!efs_query_disk_stats(old,EFS_ROOT_INO,&totals));CHECK(totals.tree_bytes==465);
            uint64_t wm[4]={1,2,3,4};CHECK(!efs_query_disk_restore(cache,wm,4<<20,&next));
            CHECK(!efs_query_disk_stats(next,EFS_ROOT_INO,&totals));CHECK(totals.tree_bytes==465);
            efs_query_disk_free(next);next=NULL;
            /* Non-dentry edits reuse the exact immutable name/alias inodes
             * and their validated digests, without a redundant sync barrier. */
            fail_sync_kind='n';sync_faults=0;
            CHECK(!efs_query_disk_build(cache,changed,old,4<<20,NULL,NULL,NULL,&next));
            CHECK(!sync_faults);fail_sync_kind=0;
            if(next){
                unsigned reused=0;
                for(unsigned shard=0;shard<SHARDS;shard++)if(next->shared_names[shard]){
                    reused++;
                    const char kinds[]={'n','a'};
                    for(unsigned j=0;j<sizeof(kinds);j++){
                        char a[EFS_MAX_PATH],b[EFS_MAX_PATH];struct stat x,y;
                        CHECK(!filename(old,shard,kinds[j],a,sizeof(a)));
                        CHECK(!filename(next,shard,kinds[j],b,sizeof(b)));
                        CHECK(!stat(a,&x) && !stat(b,&y));
                        CHECK(x.st_dev==y.st_dev && x.st_ino==y.st_ino && x.st_size==y.st_size);
                        unsigned digest=j?4:2;
                        CHECK(!memcmp(old->part[shard].digest[digest],next->part[shard].digest[digest],32));
                    }
                }
                CHECK(reused);
                CHECK(!efs_query_disk_stats(next,EFS_ROOT_INO,&totals));CHECK(totals.tree_bytes==1464);
                uint64_t newer_wm[4]={2,3,4,5};CHECK(!efs_query_disk_checkpoint(next,newer_wm));
                struct efs_query_disk *restored=NULL;
                CHECK(!efs_query_disk_restore(cache,newer_wm,4<<20,&restored));
                if(restored){
                    CHECK(!efs_query_disk_stats(restored,EFS_ROOT_INO,&totals));CHECK(totals.tree_bytes==1464);
                    efs_query_disk_free(restored);
                }
                efs_query_disk_free(next);next=NULL;
            }
            efs_kv_lsm_snapshot_release(changed);
        }
        efs_query_disk_free(old);efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
    }
    printf("query disk I/O: windows, lookup, packed records, partial ENOSPC, FD reuse and fsync refusal %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
