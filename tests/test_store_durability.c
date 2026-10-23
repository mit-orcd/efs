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
#include "../src/server/storage_roots.c"
#include "../src/server/store.c"
#include "../src/server/reclaim.c"
#include "../src/server/segment_store.c"
#include "test_tmpdir.h"
#undef open
#undef fsync
#undef close
/* Async acknowledgement on a real XFS directory ($TMPDIR): syncfs and the
 * epoch record need a real filesystem. Returns 0 when not XFS (SKIP). */
/* A valid I25 record for a 4 KiB test payload. */
static void test_record(const uint8_t *data, uint8_t rec[EFS_FRAG_RECORD_BYTES])
{
    struct efs_frag_ident id={.kind=EFS_FRAG_KIND_CHUNK,.space=1,.object=42,.generation=7,
        .payload_len=4096,.coding_profile=EFS_FRAG_PROFILE_K2F1};
    uint8_t d[EFS_HASH_SIZE];efs_hash(data,4096,d);
    assert(!efs_frag_record_encode(&id,d,rec));
}
static int async_put(struct put_sync_root *r, const char *path, uint8_t *data)
{
    uint8_t rec[EFS_FRAG_RECORD_BYTES];test_record(data,rec);
    struct shard_io_arg a={.buf=data,.len=4096,.is_write=1,.rec=rec,.direct=1,.sync_root=r};
    strcpy(a.path,path);shard_io_thread(&a);
    return a.result;
}
/* The verifier is a clean durability lineage, not just process identity.
 * Every configured root must agree before any new running record is written. */
static void test_clean_verifier_selection(void)
{
    char base[EFS_TEST_PATH];efs_test_path(base,sizeof(base),"efs-verifier-XXXXXX");assert(efs_test_mkdtemp(base));
    struct efsd_server *server=calloc(1,sizeof(*server));assert(server);
    server->storage_path_count=2;
    struct put_sync_root roots[2]={0};
    char paths[2][EFS_MAX_PATH+32];uint64_t epoch=77;
    for(unsigned i=0;i<2;i++) {
        snprintf(server->storage_paths[i],EFS_MAX_PATH,"%s/r%u",base,i);
        assert(!mkdir(server->storage_paths[i],0755));
        snprintf(paths[i],sizeof(paths[i]),"%s/%s",server->storage_paths[i],PUT_SYNC_RECORD);
        roots[i].record_fd=open(paths[i],O_CREAT|O_RDWR,0644);assert(roots[i].record_fd>=0);
        roots[i].boot=1;roots[i].verifier=101;roots[i].root_count=2;
        assert(!put_sync_record_write(&roots[i],3,1));
    }
    assert(!put_sync_reuse_epoch(server,&epoch) && epoch==101);
    roots[0].boot++;roots[1].boot++; /* arbitrarily many clean restarts */
    assert(!put_sync_record_write(&roots[0],4,1));assert(!put_sync_record_write(&roots[1],4,1));
    assert(!put_sync_reuse_epoch(server,&epoch) && epoch==101);
    assert(!put_sync_record_write(&roots[1],4,0));
    assert(!put_sync_reuse_epoch(server,&epoch) && !epoch);
    assert(!put_sync_record_write(&roots[1],4,1));
    server->storage_path_count=1;assert(!put_sync_reuse_epoch(server,&epoch) && !epoch);
    server->storage_path_count=2;roots[1].verifier=102;
    assert(!put_sync_record_write(&roots[1],4,1));assert(!put_sync_reuse_epoch(server,&epoch) && !epoch);
    roots[1].verifier=101;assert(!put_sync_record_write(&roots[1],4,1));
    uint8_t byte;assert(pread(roots[1].record_fd,&byte,1,23)==1);byte^=1;
    assert(pwrite(roots[1].record_fd,&byte,1,23)==1);
    assert(put_sync_reuse_epoch(server,&epoch)==EFS_ERR_IO && !epoch);
    assert(!put_sync_record_write(&roots[1],4,1));
    assert(!ftruncate(roots[1].record_fd,80));assert(put_sync_reuse_epoch(server,&epoch)==EFS_ERR_IO);
    char legacy[PUT_SYNC_RECORD_LEN];memset(legacy,' ',sizeof(legacy));
    int n=snprintf(legacy,sizeof(legacy),"efs-durable-epoch v1 boot=1 seq=3 time_ns=4 clean=1");
    legacy[n]=' ';legacy[sizeof(legacy)-1]='\n';
    assert(pwrite(roots[1].record_fd,legacy,sizeof(legacy),0)==sizeof(legacy));
    assert(!put_sync_reuse_epoch(server,&epoch) && !epoch);
    for(unsigned i=0;i<2;i++){close(roots[i].record_fd);assert(!unlink(paths[i]));}
    assert(!put_sync_reuse_epoch(server,&epoch) && !epoch);
    for(unsigned i=0;i<2;i++)assert(!rmdir(server->storage_paths[i]));
    free(server);assert(!rmdir(base));
}

/* A startup refused after attach (quota, listen, join...) must not leave
 * the roots marked as an unclean boot: that fences COMMIT (EIO) until W71
 * recovery, for a boot that wrote nothing. Before serving, the exit
 * handler restores the previous record; a touched or serving boot does not. */
static void reattach(struct put_sync_root *r,const char *root)
{
    close(r->fd);close(r->record_fd);memset(r,0,sizeof(*r));put_sync_state[0]=0;
    booting=1;put_sync_attach(r,root,101,1);booting=0;assert(r->fd>=0);put_sync_state[0]=1;
}
static void abandon_reset(void){put_sync_serving=0;put_sync_stopped=0;}
static void test_startup_abandon(uint8_t *data)
{
    char root[1024],path[1100],rec[1100];
    efs_test_path(root,sizeof(root),"efs-abandon-XXXXXX");assert(efs_test_mkdtemp(root));
    snprintf(rec,sizeof(rec),"%s/%s",root,PUT_SYNC_RECORD);
    struct put_sync_root *r=&put_sync_roots[0];struct put_sync_record record;int fd;
    /* Fresh root: abandoning the first boot removes the record it created. */
    abandon_reset();booting=1;put_sync_attach(r,root,101,1);booting=0;put_sync_state[0]=1;
    assert(r->fd>=0 && !r->prev_existed && !access(rec,F_OK));
    server_put_sync_abandon();assert(access(rec,F_OK) && errno==ENOENT);
    /* Clean predecessor (boot 1 stopped clean), then a refused boot 2. */
    abandon_reset();reattach(r,root);server_put_sync_stop(NULL);
    fd=open(rec,O_RDONLY);assert(fd>=0 && !put_sync_record_read(fd,&record));close(fd);
    assert(record.boot==1 && record.clean);
    abandon_reset();reattach(r,root);assert(r->boot==2 && r->prev_clean);
    fd=open(rec,O_RDONLY);assert(fd>=0 && !put_sync_record_read(fd,&record));close(fd);
    assert(record.boot==2 && !record.clean); /* attach marked boot 2 running */
    server_put_sync_abandon();
    fd=open(rec,O_RDONLY);assert(fd>=0 && !put_sync_record_read(fd,&record));close(fd);
    assert(record.boot==1 && record.clean);
    abandon_reset();reattach(r,root);
    assert(r->prev_clean && !r->recovery_uncertain && r->boot==2);
    /* Serving began: an exit is no longer provably write-free. */
    server_put_sync_serving();server_put_sync_abandon();
    abandon_reset();reattach(r,root);assert(!r->prev_clean && r->recovery_uncertain);
    /* Restore an honest clean lineage for the next case (fresh record). */
    close(r->fd);close(r->record_fd);memset(r,0,sizeof(*r));put_sync_state[0]=0;assert(!unlink(rec));
    abandon_reset();booting=1;put_sync_attach(r,root,101,1);booting=0;put_sync_state[0]=1;
    server_put_sync_stop(NULL);abandon_reset();reattach(r,root);
    /* A boot that acknowledged a PUT is never restored, even before serving. */
    snprintf(path,sizeof(path),"%s/data/a/g1",root);
    snprintf(path+strlen(root),sizeof(path)-strlen(root),"/data");mkdir(path,0755);
    snprintf(path,sizeof(path),"%s/data/a",root);mkdir(path,0755);
    snprintf(path,sizeof(path),"%s/data/a/g1",root);
    assert(async_put(r,path,data)==EFS_OK);
    server_put_sync_abandon();
    abandon_reset();reattach(r,root);assert(!r->prev_clean && r->recovery_uncertain);
    /* An orderly stop makes the exit handler a no-op. */
    put_sync_state[0]=1;server_put_sync_stop(NULL);server_put_sync_abandon();
    abandon_reset();reattach(r,root);assert(r->recovery_uncertain);
    /* A corrupt previous record is never resurrected as clean. */
    close(r->fd);close(r->record_fd);memset(r,0,sizeof(*r));put_sync_state[0]=0;
    fd=open(rec,O_WRONLY|O_TRUNC);assert(fd>=0 && write(fd,"bad",3)==3);close(fd);
    abandon_reset();booting=1;put_sync_attach(r,root,101,1);booting=0;assert(r->fd<0);
    server_put_sync_abandon();
    char got[4]={0};fd=open(rec,O_RDONLY);assert(fd>=0 && read(fd,got,3)==3);close(fd);
    assert(!memcmp(got,"bad",3));
    unlink(path);snprintf(path,sizeof(path),"%s/data/a",root);rmdir(path);
    snprintf(path,sizeof(path),"%s/data",root);rmdir(path);
    assert(!unlink(rec));assert(!rmdir(root));abandon_reset();
    puts("store durability: refused startup restores the previous record; touched, serving or stopped boots stay fenced PASS");
}

static int test_async_durability(uint8_t *data)
{
    char root[1024],path[1100],rec[1100];
    efs_test_path(root,sizeof(root),"efs-async-XXXXXX");assert(efs_test_mkdtemp(root));
    struct statfs fs;assert(!statfs(root,&fs));
    if((unsigned long)fs.f_type!=XFS_SUPER_MAGIC_){assert(!rmdir(root));return 0;}
    put_sync_window_ms=100;put_sync_window_bytes=1ull<<20;
    struct put_sync_root *r=&put_sync_roots[0];
    booting=1;put_sync_attach(r,root,101,1);booting=0;
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
    struct put_sync_record record;int fd=open(rec,O_RDONLY);assert(fd>=0);
    assert(!put_sync_record_read(fd,&record));close(fd);
    assert(record.boot==1 && record.clean && record.verifier==101 && record.roots==1);
    close(r->fd);close(r->record_fd);memset(r,0,sizeof(*r));put_sync_state[0]=0;
    /* Restart after a clean stop: next boot, no scan. */
    booting=1;put_sync_attach(r,root,101,1);booting=0;
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
        uint8_t rec[EFS_FRAG_RECORD_BYTES];test_record(data,rec);
        struct shard_io_arg a={.buf=data,.len=4096,.is_write=1,.rec=rec,.direct=1};
        strcpy(a.path,path);file_syncs=0;shard_io_thread(&a);assert(a.result==EFS_OK);
        int cf=open(path,O_WRONLY);assert(cf>=0 && pwrite(cf,"X",1,0)==1);close(cf);
    }
    booting=1;put_sync_attach(r,root,101,1);booting=0;
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
    booting=1;put_sync_attach(r,root,101,1);booting=0;
    assert(r->fd>=0 && !r->prev_clean && r->recovery_uncertain);
    assert(put_sync_commit_root(r)==EFS_ERR_IO);
    put_sync_state[0]=1;server_put_sync_stop(NULL);
    close(r->fd);close(r->record_fd);memset(r,0,sizeof(*r));put_sync_state[0]=0;
    for(int i=1;i<=5;i++){snprintf(path,sizeof(path),"%s/data/a/f%d",root,i);unlink(path);}
    snprintf(path,sizeof(path),"%s/data/a",root);assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/data",root);assert(!rmdir(path));
    /* A truncated epoch record cannot default to a clean predecessor. */
    int badfd=open(rec,O_WRONLY|O_TRUNC);assert(badfd>=0);assert(write(badfd,"bad",3)==3);close(badfd);
    put_sync_attach(r,root,101,1);assert(r->fd<0);
    assert(!unlink(rec));assert(!rmdir(root));
    return 1;
}
/* I25 / fragment format 2: no legacy reader. A root marked with format 0
 * (payload-only digest tails) or an unmarked root that already holds
 * fragment data is refused at startup; a fresh root is marked format 1. */
static void test_root_format(void)
{
    char dir[EFS_TEST_PATH];efs_test_path(dir,sizeof(dir),"efs-root-format-XXXXXX");assert(efs_test_mkdtemp(dir));
    struct efsd_server *server=calloc(1,sizeof(*server));assert(server);
    server->storage_path_count=1;strcpy(server->storage_paths[0],dir);
    booting=1;assert(!server_storage_roots_init(server));booting=0;
    assert(server_storage_root_mode(dir)==(int)EFS_STORAGE_ROOT_FORMAT);
    server_storage_roots_close(server);
    /* Rewrite the marker as format 0, with a valid marker digest. */
    char path[1100];snprintf(path,sizeof(path),"%s/.efs-engine",dir);
    uint8_t page[128];int fd=open(path,O_RDWR);assert(fd>=0 && pread(fd,page,128,0)==128);
    page[8]=0;efs_hash(page,96,page+96);assert(pwrite(fd,page,128,0)==128);close(fd);
    assert(server_storage_root_mode(dir)==-1);
    strcpy(server->storage_paths[0],dir);
    booting=1;assert(server_storage_roots_init(server)==EFS_ERR_IO);booting=0;
    assert(server_compute_usage(dir)==UINT64_MAX);
    /* Unmarked root with existing fragment data: format unknown, refused. */
    assert(!unlink(path));
    char data[1100];snprintf(data,sizeof(data),"%s/data",dir);assert(!mkdir(data,0755));
    /* An empty data/ (efsd creates it before marking) is a fresh root. */
    strcpy(server->storage_paths[0],dir);
    booting=1;assert(!server_storage_roots_init(server));booting=0;
    server_storage_roots_close(server);assert(!unlink(path));
    char sub[1200];snprintf(sub,sizeof(sub),"%s/exports",data);assert(!mkdir(sub,0755));
    strcpy(server->storage_paths[0],dir);
    booting=1;assert(server_storage_roots_init(server)==EFS_ERR_IO);booting=0;
    assert(access(path,F_OK)); /* no marker was created */
    assert(!rmdir(sub));assert(!rmdir(data));
    snprintf(path,sizeof(path),"%s/.efs-engine-lock",dir);unlink(path);
    free(server);assert(!rmdir(dir));
}
int main(void)
{
    test_clean_verifier_selection();
    /* Files permit nested roots: pinned proof ownership follows longest root. */
    struct efsd_server *routes=calloc(1,sizeof(*routes));assert(routes);
    struct target outer={0},inner={0};strcpy(outer.root,"/storage");strcpy(inner.root,"/storage/nested");
    routes->storage_path_count=2;routes->storage_roots[0]=&outer;routes->storage_roots[1]=&inner;
    assert(target_at(routes,"/storage/nested/data/body")==&inner);
    assert(target_at(routes,"/storage/data/body")==&outer);
    assert(!target_at(routes,"/storage-other/body"));
    routes->storage_roots[0]=&inner;routes->storage_roots[1]=&outer;
    assert(target_at(routes,"/storage/nested/data/body")==&inner);free(routes);
    char dir[EFS_TEST_PATH];efs_test_path(dir,sizeof(dir),"efs-store-durability-XXXXXX");assert(efs_test_mkdtemp(dir));
    char path[1024];snprintf(path,sizeof(path),"%s/a/b/body",dir);
    uint8_t *data;assert(!posix_memalign((void **)&data,4096,65536));memset(data,0x37,65536);
    uint8_t sum[EFS_FRAG_RECORD_BYTES];memset(sum,0x42,sizeof(sum)); /* opaque record */
    struct shard_io_arg a={.buf=data,.len=4096,.is_write=1,.rec=sum};strcpy(a.path,path);
    struct efsd_server *server=calloc(1,sizeof(*server));assert(server);
    assert(!pthread_mutex_init(&server->lock,NULL));
    server->storage_path_count=1;strcpy(server->storage_paths[0],dir);
    a.s=server;
    /* A configured but unsealed root cannot shorten the ACK barrier: every
     * ancestor up to "/" is synced, one per '/' in the path ($TMPDIR depth). */
    unsigned ancestors=0;for(const char *c=path;*c;c++)ancestors+=*c=='/';
    file_syncs=directory_syncs=0;shard_io_thread(&a);
    assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==ancestors);
    /* Real initialized files target, with an initially empty pinned cache. */
    booting=1;assert(!server_storage_roots_init(server));booting=0;
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
    server_storage_roots_close(server);
    booting=1;assert(!server_storage_roots_init(server));booting=0;
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
    assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==ancestors);
    server_storage_roots_close(server);
    assert(!pthread_mutex_destroy(&server->lock));free(server);
    assert(!unlink(path));snprintf(path,sizeof(path),"%s/a/b",dir);assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/a",dir);assert(!rmdir(path));snprintf(path,sizeof(path),"%s/.efs-engine",dir);assert(!unlink(path));
    snprintf(path,sizeof(path),"%s/.efs-engine-lock",dir);assert(!unlink(path));
    assert(!rmdir(dir));
    test_root_format();
    int async_ran=test_async_durability(data);
    test_startup_abandon(data);
    if(!async_ran)puts("async durability: SKIP (TMPDIR is not XFS)");
    free(data);
    puts("production PUT durability: buffered/direct/bounce file and pathname barriers, pinned ancestry, restart/recreation, retries, fault refusal and asynchronous acknowledgement PASS");
    return 0;
}
