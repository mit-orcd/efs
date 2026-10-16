/* Exercise the production cache boundary and partial-write error behavior. */
#include "efs/dir_layout.h"
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#ifdef __linux__
#include <sys/ioctl.h>
static int clone_error;
static int fault_clone(int fd,unsigned long request,int source)
{ if(clone_error){errno=clone_error;return -1;}return ioctl(fd,request,source); }
#endif
static const char *peak_path;
static uint64_t peak_bytes,*peak_charge;
static unsigned peak_overcharge,peak_samples;
static void sample_temporary_peak(void)
{
    if(!peak_path)return;
    DIR *dir=opendir(peak_path);if(!dir){peak_overcharge++;return;}
    uint64_t bytes=0;struct dirent *entry;
    while((entry=readdir(dir))){
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        char path[1024];snprintf(path,sizeof(path),"%s/%s",peak_path,entry->d_name);
        struct stat st;if(stat(path,&st) || st.st_size<0){peak_overcharge++;continue;}
        if(S_ISREG(st.st_mode))bytes+=(uint64_t)st.st_size;
    }
    closedir(dir);peak_samples++;
    if(bytes>peak_bytes)peak_bytes=bytes;
    if(bytes>*peak_charge)peak_overcharge++;
}
static unsigned fail_sort_unlink,sort_unlinks,fail_sort_close;
static int fault_unlink(const char *path)
{
    if(strstr(path,".tmp")){
        sort_unlinks++;
        if(fail_sort_unlink && !--fail_sort_unlink){errno=EIO;return -1;}
    }
    return unlink(path);
}
static int partial_write;
static int peak_enabled,sort_unlink_fault,sort_rename_fault,sort_close_fault;
static void check_sort_peak(void);
static int sort_unlink(const char *path)
{
    if(sort_unlink_fault && strstr(path,".tmp")){sort_unlink_fault=0;errno=EIO;return -1;}
    int rc=fault_unlink(path);if(peak_enabled)check_sort_peak();return rc;
}
static int sort_rename(const char *from,const char *to)
{
    if(sort_rename_fault && strstr(from,".tmp") &&
       (sort_rename_fault==1 ? strstr(to,".tmp")!=NULL : strstr(to,".tmp")==NULL)){
        sort_rename_fault=0;errno=EIO;return -1;
    }
    int rc=rename(from,to);if(peak_enabled)check_sort_peak();return rc;
}
static unsigned write_calls;
static int fail_directory_open;
static unsigned read_calls;
static unsigned alias_opens,name_opens;
static unsigned raw_syncs,index_syncs,sync_faults;
static unsigned thread_creates,fail_thread_create;
static int fault_thread_create(pthread_t *thread,const pthread_attr_t *attr,void *(*fn)(void *),void *ctx)
{
    thread_creates++;
    if(fail_thread_create && !--fail_thread_create)return EAGAIN;
    return pthread_create(thread,attr,fn,ctx);
}
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
    if((flags&O_DIRECTORY) && fail_directory_open){errno=fail_directory_open;return -1;}
    mode_t mode=0;
    if(flags&O_CREAT){va_list args;va_start(args,flags);mode=va_arg(args,int);va_end(args);}
    int fd=open(path,flags,mode);
    if(fd>=0 && fd<(int)sizeof(descriptor_kind)){
        /* Test code also closes descriptors outside the production wrapper. */
        if(writing_raw[fd]){writing_raw[fd]=0;raw_handles--;}
        size_t n=strlen(path);char kind=(flags&O_DIRECTORY)?'D':0;
        if(!(flags&O_DIRECTORY)){
            if(n>=2 && path[n-2]=='.')kind=path[n-1];
            const char *name=strrchr(path,'/');if(name && !strcmp(name+1,"manifest"))kind='m';
        }
        if(strstr(path,".tmp"))kind='T';
        descriptor_kind[fd]=kind;
        if(kind=='a')__atomic_add_fetch(&alias_opens,1,__ATOMIC_RELAXED);
        if(kind=='n')__atomic_add_fetch(&name_opens,1,__ATOMIC_RELAXED);
        if((flags&O_CREAT) && (kind=='r' || kind=='i')){
            writing_raw[fd]=1;raw_handles++;
            if(raw_handles>raw_handles_high)raw_handles_high=raw_handles;
        }
    }
    return fd;
}
static int tracked_close(int fd)
{
    int fail=fd>=0 && fd<(int)sizeof(descriptor_kind) && descriptor_kind[fd]=='T' && fail_sort_close && !--fail_sort_close;
    if(fd>=0 && fd<(int)sizeof(writing_raw)){
        if(writing_raw[fd]){writing_raw[fd]=0;raw_handles--;}
        descriptor_kind[fd]=0;
    }
    int rc=close(fd);
    if(fail || (peak_enabled && sort_close_fault && !--sort_close_fault)){errno=EIO;return -1;}
    return rc;
}
static int fault_fsync(int fd)
{
    char kind=fd>=0 && fd<(int)sizeof(descriptor_kind)?descriptor_kind[fd]:0;
    if(kind=='r')__atomic_add_fetch(&raw_syncs,1,__ATOMIC_RELAXED);
    if(kind=='i')__atomic_add_fetch(&index_syncs,1,__ATOMIC_RELAXED);
    if(kind && kind==fail_sync_kind){__atomic_add_fetch(&sync_faults,1,__ATOMIC_RELAXED);errno=EIO;return -1;}
    return fsync(fd);
}
static ssize_t counted_pread(int fd,void *buf,size_t size,off_t offset)
{ __atomic_add_fetch(&read_calls,1,__ATOMIC_RELAXED);return pread(fd,buf,size,offset); }
static ssize_t fault_pwrite_impl(int fd,const void *buf,size_t size,off_t offset)
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
    ssize_t result=pwrite(fd,buf,size,offset);
    sample_temporary_peak();return result;
}
static ssize_t fault_pwrite(int fd,const void *buf,size_t size,off_t offset)
{
    ssize_t rc=fault_pwrite_impl(fd,buf,size,offset);
    if(peak_enabled)check_sort_peak();return rc;
}
static unsigned pending_scans;
static int counted_snapshot_scan(struct efs_kv_lsm_snapshot *source,
    const uint8_t *prefix,uint32_t pn,const uint8_t *start,uint32_t sn,
    int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *user)
{
    if(pn==3 && (prefix[2]==EFS_KV_KIND_INTENT || prefix[2]==EFS_KV_KIND_GUARD || prefix[2]==EFS_KV_KIND_REDUCE))pending_scans++;
    return efs_kv_lsm_snapshot_scan(source,prefix,pn,start,sn,cb,user);
}
#define efs_kv_lsm_snapshot_scan counted_snapshot_scan
#define pthread_create fault_thread_create
#ifdef __linux__
#define ioctl fault_clone
#endif
#define unlink sort_unlink
#define rename sort_rename
#define pwrite fault_pwrite
#define pread counted_pread
#define fsync fault_fsync
#define open tracked_open
#define close tracked_close
#ifdef __linux__
#define sync_file_range fault_writeback
#endif
#include "../src/meta/query_disk.c"
#undef pthread_create
#undef efs_kv_lsm_snapshot_scan
#undef unlink
#undef rename
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
static struct efs_query_disk *peak_disk;
static char peak_root[EFS_MAX_PATH];
static uint64_t peak_file_bytes,peak_checks;
static uint64_t directory_bytes(const char *path,int recurse)
{
    uint64_t bytes=0;DIR *dir=opendir(path);CHECK(dir);if(!dir)return 0;
    struct dirent *entry;
    while((entry=readdir(dir))){
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        char file[EFS_MAX_PATH];snprintf(file,sizeof(file),"%s/%s",path,entry->d_name);
        struct stat st;CHECK(!lstat(file,&st));
        if(S_ISREG(st.st_mode))bytes+=(uint64_t)st.st_size;
        else if(S_ISDIR(st.st_mode) && recurse)bytes+=directory_bytes(file,0);
    }
    closedir(dir);return bytes;
}
static void check_sort_peak(void)
{
    uint64_t bytes=directory_bytes(peak_disk->path,0),root=directory_bytes(peak_root,1);
    CHECK(bytes<=peak_disk->disk_bytes && peak_disk->disk_bytes<=peak_disk->disk_limit);
    CHECK(root<=peak_disk->root_bytes+peak_disk->disk_bytes);
    CHECK(!peak_disk->root_limit || peak_disk->root_bytes+peak_disk->disk_bytes<=peak_disk->root_limit);
    if(bytes>peak_file_bytes)peak_file_bytes=bytes;
    peak_checks++;
}
static int sort_cancel(void *ctx){unsigned *left=ctx;return !--*left;}
static void sort_budget_test(void)
{
    unsigned before=failures;
    for(unsigned scenario=0;scenario<13;scenario++){
        unsigned count=scenario==9?0:scenario==10?1:SORT_ROWS*4+1;
        char store[]="/tmp/efs-sort-kv-XXXXXX",root[]="/tmp/efs-sort-budget-XXXXXX";
        CHECK(mkdtemp(store));CHECK(mkdtemp(root));
        struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);
        CHECK(kv);if(!kv)continue;
        for(unsigned i=0;i<count;i++){
            char name[32];snprintf(name,sizeof(name),"name-%08u",i);
            uint8_t key[EFS_KV_KEY_MAX],val[EFS_META_DENT_BYTES];uint32_t kn;
            struct efs_meta_dentry dent={UINT64_C(100000)+count-i,1,S_IFREG};
            CHECK(!efs_kv_key_dentry(1,1,name,key,&kn));CHECK(!efs_meta_pack_dentry(&dent,val,sizeof(val)));
            CHECK(!efs_kv_put(kv,key,kn,val,sizeof(val)));
        }
        struct efs_kv_lsm_snapshot *source=NULL;CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
        struct efs_query_disk *d=calloc(1,sizeof(*d));CHECK(d);if(!d)continue;
        d->budget_fd=-1;d->disk_limit=UINT64_MAX;d->disk_bytes=sizeof(d->part)+96;
        pthread_mutex_init(&d->mutex,NULL);for(unsigned i=0;i<FD_CACHE;i++)d->fds[i].fd=-1;
        snprintf(d->path,sizeof(d->path),"%s/view-XXXXXX",root);CHECK(mkdtemp(d->path));CHECK(!register_disk(d));
        struct raw_sync_batch sync={0};CHECK(!raw_build(d,source,1,&sync));CHECK(!raw_sync_finish(d,&sync,0));
        /* Retained generation is real on disk and remains unchanged while
         * the candidate sorts. root_bytes includes this old immutable view. */
        char retained[EFS_MAX_PATH],oldfile[EFS_MAX_PATH];
        snprintf(retained,sizeof(retained),"%s/view-retained",root);CHECK(!mkdir(retained,0700));
        snprintf(oldfile,sizeof(oldfile),"%s/old",retained);int oldfd=open(oldfile,O_CREAT|O_RDWR,0600);
        CHECK(oldfd>=0);if(oldfd>=0){CHECK(!ftruncate(oldfd,65536));close(oldfd);}
        d->root_bytes=65536;d->root_limit=d->root_bytes+d->disk_bytes+2*(uint64_t)count*sizeof(struct sort_name);
        uint64_t baseline=d->disk_bytes,temp=2*(uint64_t)count*sizeof(struct sort_name);
        if(scenario==1)sort_unlink_fault=1;
        if(scenario==2)sort_rename_fault=1;
        if(scenario==3)fail_write_kind='n'; /* Compact final output replaces full-name rename. */
        if(scenario==4)partial_write=1;
        unsigned calls=2;
        if(scenario==5){d->stop=sort_cancel;d->stop_ctx=&calls;}
        if(scenario==6)d->disk_limit=baseline+temp-1;
        if(scenario==7)d->root_limit=d->root_bytes+baseline+temp-1;
        if(scenario==8)d->part[1].names=UINT64_MAX;
        if(scenario==11)sort_close_fault=1; /* Initial run close. */
        if(scenario==12)sort_close_fault=8; /* First merge output close. */
        peak_disk=d;strcpy(peak_root,root);peak_file_bytes=peak_checks=0;peak_enabled=1;
        int rc=names_build(d,1,'n');peak_enabled=0;
        int expected=(scenario>=1 && scenario<=4) || scenario>=11?EFS_ERR_IO:scenario==5?EFS_ERR_BUSY:
            scenario==6 || scenario==7?EFS_ERR_QUOTA:scenario==8?EFS_ERR_OVERFLOW:EFS_OK;
        CHECK(rc==expected);
        CHECK(d->disk_bytes==baseline+(((scenario>=1 && scenario<=5) || scenario>=11)?temp:0));
        if(!rc && count){
            peak_enabled=1;CHECK(!names_build(d,1,'a'));check_sort_peak();peak_enabled=0;
            CHECK(d->disk_bytes==baseline);
            char path[EFS_MAX_PATH];CHECK(!filename(d,1,'n',path,sizeof(path)));
            int fd=open(path,O_RDONLY);CHECK(fd>=0);
            for(unsigned i=0;i<count && fd>=0;i++){
                struct efs_name_entry e;char name[32];snprintf(name,sizeof(name),"name-%08u",i);
                CHECK(!name_at(d,1,i,&e));CHECK(!strcmp(e.name,name));
            }
            if(fd>=0)close(fd);
            /* Corrupted ordinals cannot escape the immutable dentry family. */
            close_cache(d);fd=open(path,O_RDWR);CHECK(fd>=0);
            if(fd>=0){uint64_t good,bad=count;struct efs_name_entry e;
                CHECK(pread(fd,&good,8,0)==8);CHECK(pwrite(fd,&bad,8,0)==8);
                CHECK(name_at(d,1,0,&e)==EFS_ERR_PROTO);close_cache(d);
                CHECK(pwrite(fd,&good,8,0)==8);close(fd);
            }
            CHECK(!filename(d,1,'a',path,sizeof(path)));fd=open(path,O_RDONLY);CHECK(fd>=0);
            for(unsigned i=0;i<count && fd>=0;i++){
                struct disk_alias alias;CHECK(pread(fd,&alias,sizeof(alias),(off_t)i*sizeof(alias))==sizeof(alias));
                CHECK(alias.child==UINT64_C(100001)+i && alias.ordinal==count-1-i);
            }
            if(fd>=0)close(fd);
        }
        if(scenario==0)CHECK(peak_file_bytes>baseline); /* Scratch is genuinely required. */
        if(scenario==0)printf("sort peak: rows=%u measured=%llu baseline_charge=%llu checks=%llu\n",count,
            (unsigned long long)peak_file_bytes,(unsigned long long)baseline,(unsigned long long)peak_checks);
        sort_unlink_fault=sort_rename_fault=sort_close_fault=partial_write=0;d->stop=NULL;
        char path[EFS_MAX_PATH];strcpy(path,d->path);efs_query_disk_free(d);CHECK(access(path,F_OK)!=0);
        CHECK(!access(oldfile,F_OK));CHECK(!unlink(oldfile));CHECK(!rmdir(retained));
        peak_disk=NULL;efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
    }
    printf("sort budget: actual per-generation/root peaks, odd merges, checked cleanup, rename, ENOSPC, cancellation, exact quota and overflow %s\n",failures==before?"PASS":"FAIL");
}
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
struct local_page_output {unsigned emitted,limit,allow_nested;char name[8][EFS_MAX_NAME];};
static int local_page_emit(void *ctx,const struct efs_name_entry *entry,const char *path)
{
    struct local_page_output *p=ctx;
    if(p->emitted==p->limit)return EFS_ERR_AGAIN;
    CHECK(p->emitted<8 && (p->allow_nested || !strcmp(entry->name,path)));
    if(p->emitted<8)strcpy(p->name[p->emitted],path);
    p->emitted++;return EFS_OK;
}
static int local_page_busy(void *ctx,const struct efs_name_entry *entry,const char *path)
{(void)ctx;(void)entry;(void)path;return EFS_ERR_BUSY;}
static int local_page_stop(void *ctx){(void)ctx;return 1;}
static void local_page_test(void)
{
    char store[]="/tmp/efs-query-local-kv-XXXXXX",cache[]="/tmp/efs-query-local-cache-XXXXXX";
    CHECK(mkdtemp(store));CHECK(mkdtemp(cache));
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);
    CHECK(kv);if(!kv)return;
    struct efs_meta_attrs attrs={1000,1000,100};efs_ino_t leaf,outside,file;
    CHECK(!efs_meta_apply_init(kv,100));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"leaf",&leaf));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"outside",&outside));
    for(unsigned i=0;i<5;i++){char name[32];snprintf(name,sizeof(name),"report-%02u",i);CHECK(!efs_meta_apply_create_file(kv,&attrs,leaf,S_IFREG|0644,name,&file));}
    for(unsigned i=0;i<2048;i++){char name[32];snprintf(name,sizeof(name),"unrelated-%04u",i);CHECK(!efs_meta_apply_create_file(kv,&attrs,outside,S_IFREG|0644,name,&file));}
    /* Historical inactive lanes populate enough physical partitions to
     * exercise the parallel barrier threshold without changing file totals. */
    struct efs_meta_row lane_owner;CHECK(!efs_meta_apply_get_inode(kv,file,&lane_owner));
    for(unsigned lane=0;lane<64;lane++){
        uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_LANE_BYTES]={0};uint32_t kn;
        CHECK(!efs_kv_key_lane(efs_kv_lane_shard(file,(uint8_t)lane),file,lane_owner.generation,(uint8_t)lane,key,&kn));
        CHECK(!efs_kv_put(kv,key,kn,value,sizeof(value)));
    }
    struct efs_kv_lsm_snapshot *source=NULL;struct efs_query_disk *disk=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,16<<20,&source));pending_scans=0;thread_creates=0;
    CHECK(!efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&disk));
    CHECK(pending_scans==3*SHARDS); /* One complete immutable proof, not a second raw-copy scan. */
    CHECK(thread_creates==PERSIST_WORKERS);
    if(disk){
        struct efs_meta_row row;CHECK(!efs_query_disk_inode(disk,leaf,&row,NULL));CHECK(row.layout==EFS_META_LAYOUT_LOCAL);
        uint64_t watermark[4]={101,102,103,104};CHECK(!efs_query_disk_checkpoint(disk,watermark));
        struct efs_query_disk *failed=NULL,*restored=NULL;
        thread_creates=0;fail_thread_create=3;
        CHECK(efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&failed)==EFS_ERR_NOMEM && !failed);
        CHECK(!fail_thread_create && thread_creates==3); /* Started workers joined before failed-candidate cleanup. */
        fail_thread_create=0;
        CHECK(!efs_query_disk_restore(cache,watermark,4<<20,&restored));
        CHECK(restored);if(restored){struct efs_query_rollup total;CHECK(!efs_query_disk_stats(restored,leaf,&total));CHECK(total.tree_files==5);}
        efs_query_disk_free(restored);
        for(unsigned match=EFS_NAME_EXACT;match<=EFS_NAME_CONTAINS;match++){
            const char *term=match==EFS_NAME_EXACT?"report-02":match==EFS_NAME_SUFFIX?"02":"report";
            struct local_page_output output={.limit=8};uint64_t position=0;close_cache(disk);name_opens=0;
            CHECK(!efs_query_disk_page(disk,leaf,match,term,0,&position,100,NULL,NULL,local_page_emit,&output));
            CHECK(position==(UINT64_C(4096)<<32) && !name_opens);
            CHECK(output.emitted==((match==EFS_NAME_EXACT || match==EFS_NAME_SUFFIX)?1:5));
            if(match==EFS_NAME_EXACT || match==EFS_NAME_SUFFIX)CHECK(!strcmp(output.name[0],"report-02"));
        }
        uint64_t position=0,first=0;struct local_page_output one={.limit=1};
        CHECK(!efs_query_disk_page(disk,leaf,EFS_NAME_CONTAINS,"report",0,&position,1,NULL,NULL,local_page_emit,&one));
        CHECK(one.emitted==1 && !strcmp(one.name[0],"report-00") && position<(UINT64_C(4096)<<32));first=position;
        struct local_page_output replay={.limit=8};uint64_t again=0;
        CHECK(!efs_query_disk_page(disk,leaf,EFS_NAME_CONTAINS,"report",0,&again,1,NULL,NULL,local_page_emit,&replay));
        CHECK(again==first && replay.emitted==1 && !strcmp(replay.name[0],one.name[0]));
        struct local_page_output rest={.limit=8};
        CHECK(!efs_query_disk_page(disk,leaf,EFS_NAME_CONTAINS,"report",0,&position,100,NULL,NULL,local_page_emit,&rest));
        CHECK(position==(UINT64_C(4096)<<32) && rest.emitted==4 && !strcmp(rest.name[0],"report-01"));
        position=0;CHECK(efs_query_disk_page(disk,leaf,EFS_NAME_CONTAINS,"report",0,&position,100,NULL,NULL,local_page_busy,NULL)==EFS_ERR_BUSY);
        position=0;CHECK(efs_query_disk_page(disk,leaf,EFS_NAME_CONTAINS,"report",0,&position,100,local_page_stop,NULL,local_page_emit,&rest)==EFS_ERR_BUSY);
        struct local_page_output missing={.limit=8};position=0;
        CHECK(!efs_query_disk_page(disk,leaf,EFS_NAME_CONTAINS,"absent",0,&position,100,NULL,NULL,local_page_emit,&missing));
        CHECK(!missing.emitted && position==(UINT64_C(4096)<<32));
        /* A later live mutation cannot change the old stream's traversal.
         * A new catalog with descendants falls back to global names for find,
         * while an immediate LOCAL listing still uses only its parent range. */
        efs_ino_t child;CHECK(!efs_meta_apply_mkdir(kv,&attrs,leaf,S_IFDIR|0755,"child",&child));
        CHECK(!efs_meta_apply_create_file(kv,&attrs,child,S_IFREG|0644,"report-inner",&file));
        struct local_page_output unchanged={.limit=8};position=0;
        CHECK(!efs_query_disk_page(disk,leaf,EFS_NAME_CONTAINS,"report",0,&position,100,NULL,NULL,local_page_emit,&unchanged));
        CHECK(unchanged.emitted==5 && position==(UINT64_C(4096)<<32));
        struct efs_kv_lsm_snapshot *fresh=NULL;struct efs_query_disk *newer=NULL;
        CHECK(!efs_kv_lsm_snapshot_pin(kv,16<<20,&fresh));CHECK(!efs_query_disk_build(cache,fresh,disk,4<<20,NULL,NULL,NULL,&newer));
        if(newer){
            close_cache(newer);name_opens=0;position=0;struct local_page_output nested={.limit=8,.allow_nested=1};
            CHECK(!efs_query_disk_page(newer,leaf,EFS_NAME_CONTAINS,"report",0,&position,100000,NULL,NULL,local_page_emit,&nested));
            CHECK(nested.emitted==6 && position==(UINT64_C(4096)<<32) && name_opens>0);
            unsigned inner=0;for(unsigned i=0;i<nested.emitted && i<8;i++)inner+=!strcmp(nested.name[i],"child/report-inner");CHECK(inner==1);
            close_cache(newer);name_opens=0;position=0;struct local_page_output immediate={.limit=8};
            CHECK(!efs_query_disk_page(newer,leaf,EFS_NAME_CONTAINS,"report",1,&position,100,NULL,NULL,local_page_emit,&immediate));
            CHECK(immediate.emitted==5 && position==(UINT64_C(4096)<<32) && !name_opens);
        }
        efs_query_disk_free(newer);efs_kv_lsm_snapshot_release(fresh);
        printf("local page: unrelated=2048 names_index_opens=%u all match modes, bounded continuation/replay, absent and BUSY/cancel controls PASS\n",name_opens);
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

static void names_peak_test(void)
{
    const unsigned count=2*SORT_ROWS+17;
    for(unsigned kind=0;kind<2;kind++)for(unsigned mode=0;mode<7;mode++){
        char root[]="/tmp/efs-query-names-XXXXXX";CHECK(mkdtemp(root));
        struct efs_query_disk *d=calloc(1,sizeof(*d));CHECK(d);if(!d)return;
        d->budget_fd=-1;d->disk_limit=UINT64_MAX;strcpy(d->path,root);
        pthread_mutex_init(&d->mutex,NULL);
        for(unsigned i=0;i<FD_CACHE;i++)d->fds[i].fd=-1;
        struct writer w={.d=d,.shard=0,.raw=-1,.index=-1};
        w.buffer=malloc(2*RAW_WRITE_WINDOW);CHECK(w.buffer);
        for(unsigned i=0;i<count;i++){
            char name[32];snprintf(name,sizeof(name),"item-%08u",count-i);
            uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_DENT_BYTES];uint32_t kn,vn=sizeof(value);
            struct efs_meta_dentry entry={.ino=count-i,.generation=1,.type=S_IFREG};
            CHECK(!efs_kv_key_dentry(0,EFS_ROOT_INO,name,key,&kn));
            CHECK(!efs_meta_pack_dentry(&entry,value,vn));
            CHECK(!save(&w,key,kn,value,vn));
        }
        CHECK(!writer_flush(&w));close(w.raw);close(w.index);free(w.buffer);
        d->part[0].names=d->part[0].count=count;
        size_t width=kind?sizeof(struct disk_alias):sizeof(struct sort_name);
        uint64_t before=d->disk_bytes,temporary=count*width*(kind?1:2);
        /* Exact generation and aggregate boundaries admit; one byte less
         * refuses before any run exists. No global permanent scratch charge. */
        d->disk_limit=before+temporary-(mode==1);
        d->root_bytes=12345;d->root_limit=d->root_bytes+before+temporary-(mode==2);
        if(mode==3)fail_sort_unlink=1;
        if(mode==4)partial_write=1;
        if(mode==5)d->stop=cancel_budget;
        if(mode==6)fail_sort_close=1;
        sort_unlinks=0;
        peak_path=root;peak_charge=&d->disk_bytes;peak_bytes=0;peak_samples=peak_overcharge=0;
        int rc=names_build(d,0,kind?'a':'n');
        sample_temporary_peak();peak_path=NULL;
        CHECK(!peak_overcharge && peak_samples);
        if(!mode)printf("sort peak %s: actual_logical=%llu reserved=%llu samples=%u\n",
            kind?"aliases":"names",(unsigned long long)peak_bytes,
            (unsigned long long)(before+temporary),peak_samples);
        CHECK(rc==(mode==1 || mode==2?EFS_ERR_QUOTA:mode==5?EFS_ERR_BUSY:mode?EFS_ERR_IO:EFS_OK));
        CHECK(d->disk_bytes==before+((mode>=3)?temporary:0));
        CHECK(!partial_write && !fail_sort_unlink && !fail_sort_close);
        if(!mode){
            CHECK(sort_unlinks==(kind?4:5));
            char path[EFS_MAX_PATH];CHECK(!filename(d,0,kind?'a':'n',path,sizeof(path)));
            int fd=open(path,O_RDONLY);CHECK(fd>=0);
            uint8_t previous[sizeof(struct efs_name_entry)],row[sizeof(struct efs_name_entry)];
            for(unsigned i=0;i<count;i++){
                size_t stored=kind?sizeof(struct disk_alias):sizeof(uint64_t);
                CHECK(!io_at(fd,row,stored,i*stored,0));
                if(!kind){
                    uint64_t ordinal;memcpy(&ordinal,row,sizeof(ordinal));CHECK(ordinal<count);
                    struct slot slot;uint8_t key[EFS_KV_KEY_MAX],value[VALUE_MAX];
                    CHECK(!read_record(d,0,ordinal,&slot,key,value));
                    struct efs_name_entry entry;CHECK(!entry_from(key,slot.key_len,value,slot.value_len,&entry));
                    memcpy(row,&entry,sizeof(entry));stored=sizeof(entry);
                }
                if(i)CHECK((kind?alias_compare(previous,row):name_compare(previous,row))<0);
                memcpy(previous,row,stored);
            }
            close(fd);
        }
        d->stop=NULL;efs_query_disk_free(d);CHECK(access(root,F_OK)<0);
    }
    puts("name/alias sort: exact temporary peak admission, cleanup refusal, partial ENOSPC and cancellation PASS");
}

struct subtree_output { unsigned count,limit;uint64_t hash; };
static int subtree_emit(void *ctx,const struct efs_name_entry *entry,const char *path)
{
    struct subtree_output *out=ctx;
    if(out->count==out->limit)return EFS_ERR_AGAIN;
    out->count++;
    for(const unsigned char *p=(const unsigned char *)path;*p;p++)out->hash=out->hash*33+*p;
    out->hash=out->hash*33+entry->dentry.ino;return EFS_OK;
}
static int subtree_deny(void *ctx,const struct efs_name_entry *entry,const char *path)
{
    struct subtree_output *out=ctx;
    if(!out || out->count==3)return EFS_ERR_BUSY;
    return subtree_emit(out,entry,path);
}
static int subtree_cancel(void *ctx)
{ unsigned *calls=ctx;return !--*calls; }
struct subtree_job { struct efs_query_disk *disk;efs_ino_t root;struct subtree_output expected;int rc; };
static void *subtree_reader(void *ctx)
{
    struct subtree_job *job=ctx;
    for(unsigned i=0;i<16 && !job->rc;i++){
        uint64_t cursor=0;struct subtree_output out={.limit=UINT_MAX};
        job->rc=efs_query_disk_page(job->disk,job->root,EFS_NAME_CONTAINS,"",0,&cursor,512,NULL,NULL,subtree_emit,&out);
        if(!job->rc && (cursor!=(UINT64_C(4096)<<32) || out.count!=job->expected.count || out.hash!=job->expected.hash))job->rc=EFS_ERR_PROTO;
    }
    return NULL;
}
static void subtree_page_test(void)
{
    unsigned before=failures;
    char store[]="/tmp/efs-subtree-kv-XXXXXX",cache[]="/tmp/efs-subtree-cache-XXXXXX";
    CHECK(mkdtemp(store));CHECK(mkdtemp(cache));
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);
    CHECK(kv);if(!kv)return;
    struct efs_meta_attrs attrs={1000,1000,100};efs_ino_t unrelated,root,child,empty,file;
    CHECK(!efs_meta_apply_init(kv,100));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"unrelated",&unrelated));
    for(unsigned i=0;i<20000;i++){
        char name[32];snprintf(name,sizeof(name),"other-%08u.txt",i);
        CHECK(!efs_meta_apply_create_file(kv,&attrs,unrelated,S_IFREG|0644,name,&file));
    }
    const char *duplicates[]={"item-00.txt","alias.txt","child","empty"};
    for(unsigned i=0;i<4;i++)CHECK(!efs_meta_apply_create_file(kv,&attrs,unrelated,S_IFREG|0644,duplicates[i],&file));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,EFS_ROOT_INO,S_IFDIR|0755,"target",&root));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,root,S_IFDIR|0755,"child",&child));
    CHECK(!efs_meta_apply_mkdir(kv,&attrs,child,S_IFDIR|0755,"empty",&empty));
    for(unsigned i=0;i<12;i++){
        char name[32];snprintf(name,sizeof(name),"item-%02u.txt",i);
        CHECK(!efs_meta_apply_create_file(kv,&attrs,i%2?child:root,S_IFREG|0644,name,&file));
    }
    CHECK(!efs_meta_apply_link(kv,root,"item-00.txt",child,"alias.txt",200));
    CHECK(!efs_meta_apply_create_file(kv,&attrs,child,S_IFREG|0644,"gone.txt",&file));
    CHECK(!efs_meta_apply_unlink(kv,child,"gone.txt",201));
    CHECK(!efs_kv_lsm_flush(kv));
    struct efs_kv_lsm_snapshot *source=NULL;struct efs_query_disk *disk=NULL,*next=NULL;
    struct efs_query_catalog *memory=NULL;
    CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&disk));
    CHECK(!efs_query_catalog_build(source,NULL,64<<20,NULL,NULL,&memory));
    if(!disk || !memory)return;
    for(unsigned match=0;match<4;match++){
        const char *terms[]={"item-00.txt","item-",".txt",""};
        struct subtree_output oracle={.limit=UINT_MAX},actual={.limit=UINT_MAX};
        uint64_t mc=0,dc=0;
        CHECK(!efs_query_catalog_page(memory,root,match,terms[match],0,&mc,100000,NULL,NULL,subtree_emit,&oracle));
        CHECK(mc==(UINT64_C(4096)<<32));
        CHECK(!efs_query_disk_page(disk,root,match,terms[match],0,&dc,512,NULL,NULL,subtree_emit,&actual));
        CHECK(dc==(UINT64_C(4096)<<32));
        if(actual.count!=oracle.count || actual.hash!=oracle.hash)
            fprintf(stderr,"subtree mode=%u disk=%u/%llu RAM=%u/%llu\n",match,actual.count,
                (unsigned long long)actual.hash,oracle.count,(unsigned long long)oracle.hash);
        CHECK(actual.count==oracle.count && actual.hash==oracle.hash);
        uint64_t cursor=0;unsigned pages=0;actual=(struct subtree_output){.limit=UINT_MAX};
        while(cursor!=(UINT64_C(4096)<<32) && pages<8){
            uint64_t start=cursor,replay=start;
            struct subtree_output page={.limit=3,.hash=actual.hash},copy=page;
            CHECK(!efs_query_disk_page(disk,root,match,terms[match],0,&cursor,512,NULL,NULL,subtree_emit,&page));
            CHECK(!efs_query_disk_page(disk,root,match,terms[match],0,&replay,512,NULL,NULL,subtree_emit,&copy));
            CHECK(replay==cursor && page.count==copy.count && page.hash==copy.hash && cursor>start);
            actual.count+=page.count;actual.hash=page.hash;
            pages++;
        }
        if(match==3)printf("recursive subtree bounded pages: %u %s\n",pages,
            cursor==(UINT64_C(4096)<<32)?"complete":"incomplete");
        CHECK(cursor==(UINT64_C(4096)<<32) && pages<=5);
        CHECK(actual.count==oracle.count && actual.hash==oracle.hash);
    }
    uint64_t retained=0;struct subtree_output first={.limit=3};
    CHECK(!efs_query_disk_page(disk,root,EFS_NAME_CONTAINS,"",0,&retained,512,NULL,NULL,subtree_emit,&first));
    uint64_t replay=retained;struct subtree_output old_page={.limit=3};
    CHECK(!efs_query_disk_page(disk,root,EFS_NAME_CONTAINS,"",0,&replay,512,NULL,NULL,subtree_emit,&old_page));
    CHECK(!efs_meta_apply_rename(kv,root,"item-00.txt",root,"renamed.txt",202));
    efs_kv_lsm_snapshot_release(source);source=NULL;CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    CHECK(!efs_query_disk_build(cache,source,disk,4<<20,NULL,NULL,NULL,&next));
    uint64_t after=retained;struct subtree_output after_page={.limit=3};
    CHECK(!efs_query_disk_page(disk,root,EFS_NAME_CONTAINS,"",0,&after,512,NULL,NULL,subtree_emit,&after_page));
    CHECK(after==replay && after_page.count==old_page.count && after_page.hash==old_page.hash);
    uint64_t cursor=0;unsigned calls=4;struct subtree_output cancelled={.limit=3};
    CHECK(efs_query_disk_page(disk,root,EFS_NAME_CONTAINS,"",0,&cursor,512,subtree_cancel,&calls,subtree_emit,&cancelled)==EFS_ERR_BUSY);
    CHECK(!cursor && !cancelled.count);
    CHECK(efs_query_disk_page(disk,root,EFS_NAME_CONTAINS,"",0,&cursor,512,NULL,NULL,subtree_deny,NULL)==EFS_ERR_BUSY);
    CHECK(cursor<=(UINT64_C(4096)<<32));
    cursor=0;struct subtree_output denied={.limit=UINT_MAX};
    CHECK(efs_query_disk_page(disk,root,EFS_NAME_CONTAINS,"",0,&cursor,512,NULL,NULL,subtree_deny,&denied)==EFS_ERR_BUSY);
    CHECK(denied.count==3); /* A denial after output is still an error. */
    for(unsigned budget=1;budget<=61;budget+=15){
        cursor=0;struct subtree_output bounded={.limit=UINT_MAX},oracle={.limit=UINT_MAX};
        CHECK(!efs_query_disk_page(disk,root,EFS_NAME_CONTAINS,"",0,&cursor,budget,NULL,NULL,subtree_emit,&bounded));
        CHECK(cursor && cursor<=(UINT64_C(4096)<<32));
        CHECK(!efs_query_disk_page(disk,root,EFS_NAME_CONTAINS,"",0,&cursor,512,NULL,NULL,subtree_emit,&bounded));
        uint64_t full=0;CHECK(!efs_query_disk_page(disk,root,EFS_NAME_CONTAINS,"",0,&full,512,NULL,NULL,subtree_emit,&oracle));
        CHECK(cursor==(UINT64_C(4096)<<32) && bounded.count==oracle.count && bounded.hash==oracle.hash);
    }
    uint64_t watermarks[4]={9,10,11,12};struct efs_query_disk *restored=NULL;
    CHECK(next && !efs_query_disk_checkpoint(next,watermarks));
    CHECK(!efs_query_disk_restore(cache,watermarks,4<<20,&restored));
    if(restored){
        cursor=0;struct subtree_output renamed={.limit=UINT_MAX};
        CHECK(!efs_query_disk_page(restored,root,EFS_NAME_EXACT,"renamed.txt",0,&cursor,512,NULL,NULL,subtree_emit,&renamed));
        CHECK(renamed.count==1 && cursor==(UINT64_C(4096)<<32));
        efs_query_disk_free(restored);
    }
    printf("recursive subtree scratch: %zu fixed query-local bytes, no retained traversal allocation\n",sizeof(struct subtree_scan));
    uint64_t full=0;struct subtree_output expected={.limit=UINT_MAX};
    CHECK(!efs_query_catalog_page(memory,root,EFS_NAME_CONTAINS,"",0,&full,100000,NULL,NULL,subtree_emit,&expected));
    struct subtree_job jobs[8];pthread_t threads[8];unsigned created=0;
    for(unsigned i=0;i<8;i++){
        jobs[i]=(struct subtree_job){.disk=disk,.root=root,.expected=expected};
        int rc=pthread_create(&threads[i],NULL,subtree_reader,&jobs[i]);CHECK(!rc);if(rc)break;created++;
    }
    for(unsigned i=0;i<created;i++){CHECK(!pthread_join(threads[i],NULL));CHECK(!jobs[i].rc);}
    efs_query_disk_free(next);efs_query_disk_free(disk);efs_query_catalog_free(memory);
    efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
    printf("recursive subtree: unrelated catalogue, aliases/tombstone/empty dir, match modes, bounded completion, replay, retained generation, refusal and cancellation %s\n",failures==before?"PASS":"FAIL");
}
static void changed_journal_test(void)
{
    const unsigned count=2*SORT_ROWS+17,shards=FD_CACHE*2;
    for(unsigned mode=0;mode<7;mode++){
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
        if(mode==5)fail_sort_unlink=1;
        if(mode==6)fail_sort_close=1;
        int rc=changes_sort(d,state);
        CHECK(!fail_sort_unlink && !fail_sort_close);
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

static void tag_legacy(struct efs_query_disk *d)
{
    char path[EFS_MAX_PATH];snprintf(path,sizeof(path),"%s/manifest",d->path);
    int fd=open(path,O_RDWR);CHECK(fd>=0);if(fd<0)return;
    uint64_t version=5,row=sizeof(struct disk_node)+sizeof(struct efs_name_entry)-sizeof(struct disk_link);
    CHECK(pwrite(fd,&version,8,8)==8);CHECK(pwrite(fd,&row,8,24)==8);close(fd);
}
static void legacy_retirement_test(void)
{
    char store[]="/tmp/efs-query-legacy-kv-XXXXXX",cache[]="/tmp/efs-query-legacy-cache-XXXXXX";
    CHECK(mkdtemp(store));CHECK(mkdtemp(cache));
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};struct efs_kv *kv=efs_kv_lsm_open(store,&cfg);
    CHECK(kv);if(!kv)return;
    struct efs_meta_attrs attrs={1000,1000,100};efs_ino_t file;
    CHECK(!efs_meta_apply_init(kv,100));CHECK(!efs_meta_apply_create_file(kv,&attrs,EFS_ROOT_INO,S_IFREG|0644,"file",&file));
    struct efs_kv_lsm_snapshot *source=NULL;CHECK(!efs_kv_lsm_snapshot_pin(kv,8<<20,&source));
    struct efs_query_disk *old=NULL,*next=NULL,*later=NULL;uint64_t wm[4]={1,2,3,4};
    CHECK(!efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&old));
    if(!old)goto done;
    CHECK(!efs_query_disk_checkpoint(old,wm));
    uint64_t saved=aggregate_limit;aggregate_limit=2*old->disk_bytes+65536;
    char legacy[EFS_MAX_PATH];strcpy(legacy,old->path);tag_legacy(old);
    /* Deliberately tag the header while retaining v7 test rows: opaque
     * retirement must never decode the alleged old ABI. Genuine v5 rows are
     * covered by the mounted cross-build transition reproduction. */
    efs_query_disk_free(old);old=NULL;CHECK(!access(legacy,F_OK));
    CHECK(!efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&next));
    if(next){
        for(unsigned i=0;i<2;i++){
            fail_directory_open=i?EIO:EMFILE;
            CHECK(efs_query_disk_checkpoint(next,wm)==EFS_ERR_IO);
            fail_directory_open=0;
            char current[EFS_MAX_PATH],target[256];snprintf(current,sizeof(current),"%s/current",cache);
            ssize_t length=readlink(current,target,sizeof(target)-1);CHECK(length>0);
            if(length>0){target[length]=0;CHECK(!strcmp(target,strrchr(legacy,'/')+1));}
            CHECK(!access(legacy,F_OK));
        }
        fail_sync_kind='m';CHECK(efs_query_disk_checkpoint(next,wm)==EFS_ERR_IO);fail_sync_kind=0;
        CHECK(!access(legacy,F_OK)); /* Failed publication preserves old current. */
        CHECK(!efs_query_disk_checkpoint(next,wm));CHECK(access(legacy,F_OK)!=0);
        /* The same tight aggregate budget now admits the second refresh;
         * a leaked legacy generation would make this three generations. */
        CHECK(!efs_query_disk_build(cache,source,next,4<<20,NULL,NULL,NULL,&later));
        if(later){
            CHECK(!efs_query_disk_checkpoint(later,wm));efs_query_disk_free(next);next=NULL;
            strcpy(legacy,later->path);tag_legacy(later);
            CHECK(!efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&next));
            if(next){
                CHECK(!efs_query_disk_checkpoint(next,wm));CHECK(!access(legacy,F_OK));
                struct efs_query_rollup total;CHECK(!efs_query_disk_stats(later,EFS_ROOT_INO,&total) && total.tree_files==1);
                efs_query_disk_free(later);later=NULL;CHECK(access(legacy,F_OK)!=0);
            }
        }
    }
    /* A failed parent-directory fsync happens after current has switched.
     * Keep the opaque owner through retry/abandonment so it cannot leak. */
    for(unsigned mode=0;mode<2 && next;mode++){
        char abandoned[EFS_MAX_PATH];strcpy(abandoned,next->path);tag_legacy(next);
        efs_query_disk_free(next);next=NULL;
        CHECK(!efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&next));
        if(next){
            fail_sync_kind='D';CHECK(efs_query_disk_checkpoint(next,wm)==EFS_ERR_IO);fail_sync_kind=0;
            CHECK(!access(abandoned,F_OK));
            if(!mode){CHECK(!efs_query_disk_checkpoint(next,wm));CHECK(access(abandoned,F_OK)!=0);}
            else {
                efs_query_disk_free(next);next=NULL;CHECK(!access(abandoned,F_OK));
                CHECK(efs_query_disk_restore(cache,wm,4<<20,&next)==EFS_ERR_PROTO && !next);
                CHECK(!efs_query_disk_build(cache,source,NULL,4<<20,NULL,NULL,NULL,&next));
                if(next){CHECK(!efs_query_disk_checkpoint(next,wm));CHECK(access(abandoned,F_OK)!=0);}

            }
        }
    }
    aggregate_limit=saved;
done:
    efs_query_disk_free(old);efs_query_disk_free(next);efs_query_disk_free(later);
    efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
    printf("legacy retirement: failed publication keeps current, opaque replacement, retained readers and tight-budget second refresh %s\n",failures?"FAIL":"PASS");
}
int main(void)
{
    sort_budget_test();
    legacy_retirement_test();
    raw_sync_batch_test();
    orphan_preflight_test();
    alias_batch_test();
    changed_journal_test();
    pending_preflight_test();
    rebuild_cache_test();
    local_page_test();
    subtree_page_test();
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
    names_peak_test();
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
                CHECK(sync_faults>=1 && sync_faults<=PERSIST_WORKERS);fail_sync_kind=0;
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
    printf("query disk I/O: windows, lookup, encoded records, partial ENOSPC, FD reuse and fsync refusal %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
