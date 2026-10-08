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
static int fail_file,fail_directory,interrupt_directory,fail_directory_close;
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
        assert(file_syncs); /* never publish the name before fencing bytes */
        if(interrupt_directory) {interrupt_directory--;errno=EINTR;return -1;}
        if(fail_directory) {errno=EIO;return -1;}
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
#include "../src/server/store.c"
#undef open
#undef fsync
#undef close
int main(void)
{
    char dir[]="/tmp/efs-store-durability-XXXXXX";assert(mkdtemp(dir));
    char path[1024];snprintf(path,sizeof(path),"%s/a/b/body",dir);
    uint8_t *data;assert(!posix_memalign((void **)&data,4096,65536));memset(data,0x37,65536);
    uint8_t sum[EFS_HASH_SIZE];memset(sum,0x42,sizeof(sum));
    struct shard_io_arg a={.buf=data,.len=4096,.is_write=1,.sum=sum};strcpy(a.path,path);
    for(int direct=0;direct<2;direct++) for(int bounce=0;bounce<2;bounce++) {
        a.direct=direct;a.buf=data+bounce;
        for(int attempt=0;attempt<2;attempt++) {
            file_syncs=directory_syncs=0;shard_io_thread(&a);
            assert(a.result==EFS_OK && file_syncs==1 && directory_syncs>=3 && !sync_opens);
        }
        file_syncs=directory_syncs=0;fail_file=1;shard_io_thread(&a);
        assert(a.result==EFS_ERR_IO && file_syncs==1 && !directory_syncs);fail_file=0;
        file_syncs=directory_syncs=0;fail_directory=1;shard_io_thread(&a);
        assert(a.result==EFS_ERR_IO && file_syncs==1 && directory_syncs==1);fail_directory=0;
        file_syncs=directory_syncs=0;interrupt_directory=1;shard_io_thread(&a);
        assert(a.result==EFS_OK && file_syncs==1 && directory_syncs>=4);
        file_syncs=directory_syncs=0;fail_directory_close=1;shard_io_thread(&a);
        assert(a.result==EFS_ERR_IO && file_syncs==1 && directory_syncs==1);fail_directory_close=0;
    }
    assert(!unlink(path));snprintf(path,sizeof(path),"%s/a/b",dir);assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/a",dir);assert(!rmdir(path));assert(!rmdir(dir));free(data);
    puts("production PUT durability: buffered/direct/bounce file and pathname barriers, retries and fault refusal PASS");
    return 0;
}
