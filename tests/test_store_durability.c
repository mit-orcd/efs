/* Real production shard finalization: every path fences data and names.
 * The benchmark-only O_SYNC option remains outside production flags. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/vfs.h>
static unsigned file_syncs,directory_syncs,sync_opens;
static int booting;
static int fail_file,fail_directory,fail_directory_at,interrupt_directory,fail_directory_close;
static int barrier_open(const char *path,int flags,...)
{
    int mode=0;
    if(flags&O_CREAT) {va_list ap;va_start(ap,flags);mode=va_arg(ap,int);va_end(ap);}
    if((flags&O_SYNC)==O_SYNC)sync_opens++;
    return open(path,flags,mode);
}
static int barrier_sync(int fd)
{
    struct stat st;assert(!fstat(fd,&st));
    if(S_ISDIR(st.st_mode)) {
        directory_syncs++;
        assert(booting || file_syncs); /* never publish the name before fencing bytes */
        if(interrupt_directory) {interrupt_directory--;errno=EINTR;return -1;}
        if(fail_directory || directory_syncs==(unsigned)fail_directory_at) {errno=EIO;return -1;}
    } else {
        file_syncs++;
        if(fail_file) {errno=EIO;return -1;}
    }
    return fsync(fd);
}
static int barrier_close(int fd)
{
    struct stat st;assert(!fstat(fd,&st));int rc=close(fd);
    if(fail_directory_close && S_ISDIR(st.st_mode)) {errno=EIO;return -1;}
    return rc;
}
#define open barrier_open
#define fsync barrier_sync
#define close barrier_close
#include "../src/server/store_containers.c"
#include "../src/server/store.c"
#undef open
#undef fsync
#undef close
/* Async acknowledgement on a real XFS directory ($TMPDIR): syncfs and the
 * epoch record need a real filesystem. Returns 0 when not XFS (SKIP). */
static int async_put(struct put_sync_root *r, const char *path, uint8_t *data)
{
    uint8_t sum[EFS_HASH_SIZE];efs_hash(data,4096,sum);
    struct shard_io_arg a={.buf=data,.len=4096,.is_write=1,.sum=sum,.direct=1,.sync_root=r};
    strcpy(a.path,path);shard_io_thread(&a);
    return a.result;
}
static int test_async_durability(uint8_t *data)
{
    const char *tmp=getenv("TMPDIR");char root[1024],path[1100],rec[1100];
    snprintf(root,sizeof(root),"%s/efs-async-XXXXXX",tmp&&*tmp?tmp:"/tmp");assert(mkdtemp(root));
    struct statfs fs;assert(!statfs(root,&fs));
    if((unsigned long)fs.f_type!=XFS_SUPER_MAGIC_){assert(!rmdir(root));return 0;}
    put_sync_window_ms=100;put_sync_window_bytes=1ull<<20;
    struct put_sync_root *r=&put_sync_roots[0];
    booting=1;put_sync_attach(r,root);booting=0;
    assert(r->fd>=0 && r->boot==1 && r->prev_clean && r->seq==1 && !r->durable_seq);
    put_sync_state[0]=1;
    snprintf(rec,sizeof(rec),"%s/%s",root,PUT_SYNC_RECORD);assert(!access(rec,F_OK));
    /* New fragment: acknowledged with no fsync, counted unsynced. */
    snprintf(path,sizeof(path),"%s/data/a/f1",root);
    file_syncs=directory_syncs=0;assert(async_put(r,path,data)==EFS_OK);
    assert(!file_syncs && !directory_syncs && r->cur_frags+r->inflight_frags==1);
    /* COMMIT: one barrier makes it durable. */
    assert(put_sync_commit_root(r)==EFS_OK && r->durable_seq>=1 && !r->cur_frags && !r->inflight_frags);
    uint64_t b=r->barriers;assert(b>=1);
    /* Nothing new: COMMIT returns without a barrier. */
    assert(put_sync_commit_root(r)==EFS_OK && r->barriers==b);
    /* Overwrite of an existing name is asynchronous too; the barrier's
     * record fdatasync flushes the device cache for it. */
    file_syncs=directory_syncs=0;assert(async_put(r,path,data)==EFS_OK);
    assert(!file_syncs && !directory_syncs && r->cur_frags+r->inflight_frags==1);
    assert(put_sync_commit_root(r)==EFS_OK && !r->cur_frags && !r->inflight_frags);
    b=r->barriers;
    /* The window alone triggers a barrier (no COMMIT). */
    snprintf(path,sizeof(path),"%s/data/a/f2",root);assert(async_put(r,path,data)==EFS_OK);
    for(int i=0;i<200 && (r->cur_frags||r->inflight_frags);i++)usleep(10000);
    assert(!r->cur_frags && !r->inflight_frags && r->barriers>b && r->synced_bytes==r->acked_bytes);
    /* Clean stop marks the record clean. */
    server_put_sync_stop(NULL);
    char text[256]={0};int fd=open(rec,O_RDONLY);assert(fd>=0 && read(fd,text,sizeof(text)-1)>0);close(fd);
    assert(strstr(text,"boot=1") && strstr(text,"clean=1"));
    close(r->fd);close(r->record_fd);memset(r,0,sizeof(*r));put_sync_state[0]=0;
    /* Restart after a clean stop: next boot, no scan. */
    booting=1;put_sync_attach(r,root);booting=0;
    assert(r->fd>=0 && r->boot==2 && r->prev_clean && r->scan_done);
    put_sync_state[0]=1;
    /* A failed barrier is sticky until restart: COMMIT fails even after a
     * later barrier succeeds; PUTs stay asynchronous. */
    snprintf(path,sizeof(path),"%s/data/a/f3",root);assert(async_put(r,path,data)==EFS_OK);
    int saved=r->fd;r->fd=-1;
    assert(put_sync_commit_root(r)==EFS_ERR_IO && r->error && r->failures>=1);
    r->fd=saved;
    snprintf(path,sizeof(path),"%s/data/a/f4",root);
    file_syncs=directory_syncs=0;assert(async_put(r,path,data)==EFS_OK);
    assert(!file_syncs && !directory_syncs);
    assert(put_sync_commit_root(r)==EFS_ERR_IO);
    server_put_sync_stop(NULL);
    close(r->fd);close(r->record_fd);memset(r,0,sizeof(*r));put_sync_state[0]=0;
    /* Unclean previous stop (the failed barrier left clean=0): the scan
     * verifies fragments modified since and reports a corrupted one. */
    snprintf(path,sizeof(path),"%s/data/a/f5",root);
    {   /* f5: a valid fragment, then its payload corrupted */
        uint8_t sum[EFS_HASH_SIZE];efs_hash(data,4096,sum);
        struct shard_io_arg a={.buf=data,.len=4096,.is_write=1,.sum=sum,.direct=1};
        strcpy(a.path,path);file_syncs=0;shard_io_thread(&a);assert(a.result==EFS_OK);
        int cf=open(path,O_WRONLY);assert(cf>=0 && pwrite(cf,"X",1,0)==1);close(cf);
    }
    booting=1;put_sync_attach(r,root);booting=0;
    assert(r->fd>=0 && r->boot==3 && !r->prev_clean && r->scanner_started);
    pthread_join(r->scanner,NULL);r->scanner_started=0;
    assert(r->scan_done && r->scanned>=4 && r->bad==1);
    /* A fresh client must not mistake an empty new-boot counter for durable
     * old data. Even a successful new barrier cannot clear uncertainty. */
    assert(r->recovery_uncertain && put_sync_commit_root(r)==EFS_ERR_IO);
    assert(async_put(r,path,data)==EFS_OK);
    assert(put_sync_commit_root(r)==EFS_ERR_IO);
    put_sync_state[0]=1;server_put_sync_stop(NULL);
    close(r->fd);close(r->record_fd);memset(r,0,sizeof(*r));put_sync_state[0]=0;
    booting=1;put_sync_attach(r,root);booting=0;
    assert(r->fd>=0 && !r->prev_clean && r->recovery_uncertain);
    assert(put_sync_commit_root(r)==EFS_ERR_IO);
    put_sync_state[0]=1;server_put_sync_stop(NULL);
    close(r->fd);close(r->record_fd);memset(r,0,sizeof(*r));put_sync_state[0]=0;
    for(int i=1;i<=5;i++){snprintf(path,sizeof(path),"%s/data/a/f%d",root,i);unlink(path);}
    snprintf(path,sizeof(path),"%s/data/a",root);assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/data",root);assert(!rmdir(path));
    /* A truncated epoch record cannot default to a clean predecessor. */
    int badfd=open(rec,O_WRONLY|O_TRUNC);assert(badfd>=0);assert(write(badfd,"bad",3)==3);close(badfd);
    put_sync_attach(r,root);assert(r->fd<0);
    assert(!unlink(rec));assert(!rmdir(root));
    return 1;
}
int main(void)
{
    /* Files permit nested roots: pinned proof ownership follows longest root. */
    struct efsd_server *routes=calloc(1,sizeof(*routes));assert(routes);
    struct target outer={0},inner={0};strcpy(outer.root,"/storage");strcpy(inner.root,"/storage/nested");
    routes->storage_path_count=2;routes->container_targets[0]=&outer;routes->container_targets[1]=&inner;
    assert(target_at(routes,"/storage/nested/data/body")==&inner);
    assert(target_at(routes,"/storage/data/body")==&outer);
    assert(!target_at(routes,"/storage-other/body"));
    routes->container_targets[0]=&inner;routes->container_targets[1]=&outer;
    assert(target_at(routes,"/storage/nested/data/body")==&inner);free(routes);
    char dir[]="/tmp/efs-store-durability-XXXXXX";assert(mkdtemp(dir));
    char path[1024];snprintf(path,sizeof(path),"%s/a/b/body",dir);
    uint8_t *data;assert(!posix_memalign((void **)&data,4096,65536));memset(data,0x37,65536);
    uint8_t sum[EFS_HASH_SIZE];memset(sum,0x42,sizeof(sum));
    struct shard_io_arg a={.buf=data,.len=4096,.is_write=1,.sum=sum};strcpy(a.path,path);
    struct efsd_server *server=calloc(1,sizeof(*server));assert(server);
    assert(!pthread_mutex_init(&server->lock,NULL));
    server->storage_path_count=1;strcpy(server->storage_paths[0],dir);
    a.s=server;
    /* A configured but unsealed root cannot shorten the ACK barrier. */
    file_syncs=directory_syncs=0;shard_io_thread(&a);
    assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==5);
    /* Real initialized files target, with an initially empty pinned cache. */
    booting=1;assert(!server_storage_engine_init(server));booting=0;
    file_syncs=directory_syncs=0;shard_io_thread(&a);
    assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==3);
    file_syncs=directory_syncs=0;shard_io_thread(&a);
    assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==1);
    for(int direct=0;direct<2;direct++) for(int bounce=0;bounce<2;bounce++) {
        a.direct=direct;a.buf=data+bounce;
        for(int attempt=0;attempt<2;attempt++) {
            file_syncs=directory_syncs=0;shard_io_thread(&a);
            assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==1 && !sync_opens);
        }
        file_syncs=directory_syncs=0;fail_file=1;shard_io_thread(&a);
        assert(a.result==EFS_ERR_IO && file_syncs==1 && !directory_syncs);fail_file=0;
        file_syncs=directory_syncs=0;fail_directory=1;shard_io_thread(&a);
        assert(a.result==EFS_ERR_IO && file_syncs==1 && directory_syncs==1);fail_directory=0;

        file_syncs=directory_syncs=0;shard_io_thread(&a);
        assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==1);
        file_syncs=directory_syncs=0;interrupt_directory=1;shard_io_thread(&a);
        assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==2);
        file_syncs=directory_syncs=0;fail_directory_close=1;shard_io_thread(&a);
        assert(a.result==EFS_ERR_IO && file_syncs==1 && directory_syncs==1);fail_directory_close=0;
    }
    /* Short TCP worker lifetimes must retire both direct-I/O cache pages.
     * Valgrind sees lost buffers after join if either TLS destructor is absent. */
    for (int i=0;i<32;i++) {
        pthread_t thread;a.direct=1;a.buf=data+(i%2);
        file_syncs=directory_syncs=0;
        assert(!pthread_create(&thread,NULL,shard_io_thread,&a));
        assert(!pthread_join(thread,NULL));
        assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==1);
    }
    /* Restart discards runtime hints; first publication reproves ancestry. */
    server_storage_engine_close(server);
    booting=1;assert(!server_storage_engine_init(server));booting=0;
    file_syncs=directory_syncs=0;shard_io_thread(&a);
    assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==3);
    file_syncs=directory_syncs=0;shard_io_thread(&a);
    assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==1);
    /* Failure on an unknown ancestor must not admit any hint. */
    snprintf(a.path,sizeof(a.path),"%s/new/deep/body",dir);
    file_syncs=directory_syncs=0;fail_directory_at=3;shard_io_thread(&a);
    assert(a.result==EFS_ERR_IO && directory_syncs==3);fail_directory_at=0;
    file_syncs=directory_syncs=0;shard_io_thread(&a);
    assert(a.result==EFS_OK && directory_syncs==3);
    file_syncs=directory_syncs=0;shard_io_thread(&a);
    assert(a.result==EFS_OK && directory_syncs==1);
    /* A deleted/recreated directory cannot reuse the pinned inode proof. */
    assert(!unlink(a.path));char sub[1024];snprintf(sub,sizeof(sub),"%s/new/deep",dir);assert(!rmdir(sub));assert(!mkdir(sub,0755));
    file_syncs=directory_syncs=0;shard_io_thread(&a);
    assert(a.result==EFS_OK && directory_syncs==2);
    assert(!unlink(a.path));assert(!rmdir(sub));snprintf(sub,sizeof(sub),"%s/new",dir);assert(!rmdir(sub));
    strcpy(a.path,path);
    /* A textual prefix is not an initialized ancestor; retain all barriers. */
    server->storage_paths[0][strlen(dir)-1]=0;
    file_syncs=directory_syncs=0;shard_io_thread(&a);
    assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==5);
    server_storage_engine_close(server);
    assert(!pthread_mutex_destroy(&server->lock));free(server);
    assert(!unlink(path));snprintf(path,sizeof(path),"%s/a/b",dir);assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/a",dir);assert(!rmdir(path));snprintf(path,sizeof(path),"%s/.efs-engine",dir);assert(!unlink(path));
    snprintf(path,sizeof(path),"%s/.efs-engine-lock",dir);assert(!unlink(path));
    assert(!rmdir(dir));
    int async_ran=test_async_durability(data);
    if(!async_ran)puts("async durability: SKIP (TMPDIR is not XFS)");
    free(data);
    puts("production PUT durability: buffered/direct/bounce file and pathname barriers, pinned ancestry, restart/recreation, retries, fault refusal and asynchronous acknowledgement PASS");
    return 0;
}
