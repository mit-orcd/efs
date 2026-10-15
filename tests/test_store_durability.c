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
static unsigned cohort_barriers;
static int cohort_fail;
static int test_cohort_barrier(int fd)
{
    (void)fd;cohort_barriers++;
    return cohort_fail?-1:0;
}
#define open barrier_open
#define fsync barrier_sync
#define close barrier_close
#include "../src/server/store_containers.c"
#include "../src/server/store.c"
#undef open
#undef fsync
#undef close
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
    /* Opt-in grouped durability: a fragment this PUT creates joins its
     * root's cohort with no file or directory fsync; an existing name keeps
     * both. A failed cohort barrier fails the PUT; its retry overwrites. */
    {
        struct durable_cohort tc=DURABLE_COHORT_INIT;tc.barrier=test_cohort_barrier;
        char cpath[1024],cdir[1024];snprintf(cpath,sizeof(cpath),"%s/cohort/x/body",dir);
        struct shard_io_arg ca={.buf=data,.len=4096,.is_write=1,.sum=sum,.direct=1,.s=server,.cohort=&tc};
        strcpy(ca.path,cpath);
        file_syncs=directory_syncs=0;shard_io_put(&ca);
        assert(ca.result==EFS_OK && ca.created && ca.cohort_joined && !file_syncs && !directory_syncs);
        assert(cohort_barriers==1 && tc.members==1 && !tc.writing);
        file_syncs=directory_syncs=0;shard_io_put(&ca);
        assert(ca.result==EFS_OK && !ca.created && !ca.cohort_joined && file_syncs==1 && directory_syncs>=1);
        assert(cohort_barriers==1 && !tc.writing);
        assert(!unlink(cpath));cohort_fail=1;file_syncs=directory_syncs=0;shard_io_put(&ca);
        assert(ca.result==EFS_ERR_IO && ca.created && !file_syncs && cohort_barriers==2 && tc.failures==1 && !tc.writing);
        cohort_fail=0;file_syncs=directory_syncs=0;shard_io_put(&ca);
        assert(ca.result==EFS_OK && !ca.created && file_syncs==1 && cohort_barriers==2);
        /* An open failure never joins; its writing count is released. */
        snprintf(ca.path,sizeof(ca.path),"%s/under-a-file",cpath);
        shard_io_put(&ca);assert(ca.result==EFS_ERR_IO && !ca.cohort_joined && !tc.writing && cohort_barriers==2);
        /* No root is selected without EFS_PUT_COHORT=1. */
        assert(!getenv("EFS_PUT_COHORT") && !fragment_cohort_for(server,cpath));
        assert(!unlink(cpath));snprintf(cdir,sizeof(cdir),"%s/cohort/x",dir);assert(!rmdir(cdir));
        snprintf(cdir,sizeof(cdir),"%s/cohort",dir);assert(!rmdir(cdir));
    }
    /* A textual prefix is not an initialized ancestor; retain all barriers. */
    server->storage_paths[0][strlen(dir)-1]=0;
    file_syncs=directory_syncs=0;shard_io_thread(&a);
    assert(a.result==EFS_OK && file_syncs==1 && directory_syncs==5);
    server_storage_engine_close(server);
    assert(!pthread_mutex_destroy(&server->lock));free(server);
    assert(!unlink(path));snprintf(path,sizeof(path),"%s/a/b",dir);assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/a",dir);assert(!rmdir(path));snprintf(path,sizeof(path),"%s/.efs-engine",dir);assert(!unlink(path));
    snprintf(path,sizeof(path),"%s/.efs-engine-lock",dir);assert(!unlink(path));
    assert(!rmdir(dir));free(data);
    puts("production PUT durability: buffered/direct/bounce file and pathname barriers, pinned ancestry, restart/recreation, retries, fault refusal and opt-in cohort PASS");
    return 0;
}
