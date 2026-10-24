/* Actual shard I/O, with syscall failure injection at the final length fence. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
static int sync_opens, fail_sync, sync_calls;
static int fail_close, close_calls;
static int test_close(int fd)
{
    close_calls++;
    int rc = close(fd);
    if (fail_close) { errno = EIO; return -1; }
    return rc;
}
static int (*open_hook)(const char *, int, int);
static int test_open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if ((flags & O_SYNC) == O_SYNC) sync_opens++;
    return open_hook ? open_hook(path, flags, mode) : open(path, flags, mode);
}
static int test_fsync(int fd)
{
    sync_calls++;
    if (fail_sync) { errno = EIO; return -1; }
    return fsync(fd);
}
static int fail_finish, fail_write, fail_stat, interrupt_stat, interrupt_finish, finish_calls, stat_calls, last_fd;
static int test_fstat(int fd, struct stat *st)
{
    stat_calls++; last_fd = fd;
    if (interrupt_stat) { interrupt_stat--; errno = EINTR; return -1; }
    if (fail_stat) { errno = EIO; return -1; }
    return fstat(fd, st);
}
static int test_ftruncate(int fd, off_t length)
{
    finish_calls++; last_fd = fd;
    if (interrupt_finish) { interrupt_finish--; errno = EINTR; return -1; }
    if (fail_finish) { errno = EIO; return -1; }
    return ftruncate(fd, length);
}
static ssize_t test_write(int fd, const void *buf, size_t n)
{
    if (fail_write) { errno = EIO; return -1; }
    return write(fd, buf, n);
}
static ssize_t test_writev(int fd, const struct iovec *iov, int n)
{
    if (fail_write) { errno = EIO; return -1; }
    return writev(fd, iov, n);
}
#define close test_close
#define EFS_BENCH_BUILD
#define open test_open
#define fsync test_fsync
#define fstat test_fstat
#define ftruncate test_ftruncate
#define write test_write
#define writev test_writev
#include "../src/server/store.c"
#include "../src/server/reclaim.c"
#include "../src/server/segment_store.c"
#include "test_tmpdir.h"
#undef close
#undef open
#undef fsync
#undef fstat
#undef ftruncate
#undef write
#undef writev

static void image(const char *path, size_t bytes)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600); assert(fd >= 0);
    uint8_t buf[4096]; memset(buf, 0x99, sizeof(buf));
    for (size_t n = 0; n < bytes; n += sizeof(buf))
        assert(write(fd, buf, bytes - n < sizeof(buf) ? bytes - n : sizeof(buf)) > 0);
    close(fd);
}
/* The one stored layout (D42): payload zero-padded to 4 KiB, then a 4 KiB
 * tail page that starts with the record. */
static void check(const char *path, const uint8_t *body, size_t len,
                  const uint8_t *sum)
{
    struct stat st; assert(stat(path, &st) == 0);
    size_t span = (len + 4095) & ~(size_t)4095; if (span < 4096) span = 4096;
    size_t expected = span + (sum ? 4096 : 0);
    assert((size_t)st.st_size == expected);
    uint8_t *all = malloc(expected); assert(all);
    int fd = open(path, O_RDONLY); assert(fd >= 0);
    assert(read(fd, all, expected) == (ssize_t)expected); close(fd);
    assert(memcmp(all, body, len) == 0);
    for (size_t n = len; n < span; n++) assert(all[n] == 0);
    if (sum) {
        assert(memcmp(all + span, sum, EFS_FRAG_RECORD_BYTES) == 0);
        for (size_t n = span + EFS_FRAG_RECORD_BYTES; n < expected; n++) assert(all[n] == 0);
    }
    free(all);
}
/* Every fragment open carries O_DIRECT; there is no buffered path. */
static int direct_opens, plain_opens;
static int count_open(const char *path, int flags, int mode)
{
    if (flags & O_DIRECT) direct_opens++; else plain_opens++;
    return open(path, flags, mode);
}
int main(void)
{
    char dir[EFS_TEST_PATH];efs_test_path(dir,sizeof(dir),"efs-overwrite-XXXXXX"); assert(efs_test_mkdtemp(dir));
    char path[1024]; snprintf(path, sizeof(path), "%s/body", dir);
    uint8_t *aligned; assert(posix_memalign((void **)&aligned, 4096, 65536) == 0);
    memset(aligned, 0x37, 65536); uint8_t sum[EFS_FRAG_RECORD_BYTES]; memset(sum, 0x42, sizeof(sum)); /* opaque record bytes */
    struct shard_io_arg a = {0}; strcpy(a.path, path); a.buf = aligned;
    a.is_write = 1; a.rec = sum; a.len = 65536;
    open_hook = count_open;
    image(path, 90000); shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, a.len, sum);
    assert(direct_opens == 1 && plain_opens == 0); open_hook = NULL;
    a.len = 4096; shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, a.len, sum);
    a.rec = NULL; a.len = 8192; shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, a.len, NULL);
    /* Small and unaligned payloads stay exact through the bounce buffer. */
    a.rec = sum; a.buf = aligned + 1; a.len = 4096; /* unaligned source */
    shard_io_thread(&a); assert(a.result == EFS_OK); check(path, a.buf, a.len, sum);
    a.buf = aligned; a.len = 1003; shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, a.len, sum);
    a.len = 7003; shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, a.len, sum);
    a.len = 8192; a.src_len = 10; shard_io_thread(&a); assert(a.result == EFS_OK);
    { uint8_t *ten = calloc(1, 8192); memcpy(ten, aligned, 10); check(path, ten, 8192, sum); free(ten); }
    a.src_len = 0;
    /* Reads: the payload comes back exact, a missing tail is zero. */
    {
        uint8_t back[1003]; a.is_write = 0; a.buf = back; a.len = 1003;
        shard_io_thread(&a); assert(a.result == EFS_OK && !memcmp(back, aligned, 10));
        a.is_write = 1; a.buf = aligned;
    }
    a.len = 1003; a.rec = sum;
    fail_finish = 1; finish_calls = 0; image(path, 90000); shard_io_thread(&a);
    assert(a.result == EFS_ERR_IO && finish_calls == 1);
    assert(fcntl(last_fd, F_GETFD) == -1 && errno == EBADF);
    fail_finish = 0; interrupt_finish = 1; finish_calls = 0; image(path, 90000); shard_io_thread(&a);
    assert(a.result == EFS_OK && finish_calls == 2); check(path, aligned, a.len, sum);
    finish_calls = 0; stat_calls = 0; interrupt_stat = 1; shard_io_thread(&a);
    assert(a.result == EFS_OK && stat_calls == 2 && finish_calls == 0);
    fail_stat = 1; shard_io_thread(&a);
    assert(a.result == EFS_ERR_IO && finish_calls == 0);
    assert(fcntl(last_fd, F_GETFD) == -1 && errno == EBADF); fail_stat = 0;
    fail_write = 1; finish_calls = 0; shard_io_thread(&a);
    assert(a.result == EFS_ERR_IO && finish_calls == 0); fail_write = 0;
    check(path, aligned, a.len, sum); /* failed before write: prior image survives */
    /* Benchmark O_SYNC: O_DIRECT|O_SYNC persists the write; only a length
     * change needs another barrier. */
    a.sync_write = 1; sync_opens = 0; a.len = 4096;
    image(path, 90000); sync_calls = 0;
    shard_io_thread(&a); assert(a.result == EFS_OK && sync_opens == 1 && sync_calls == 1);
    check(path, aligned, a.len, sum);
    finish_calls = 0; sync_calls = 0; fail_sync = 1;
    shard_io_thread(&a); assert(a.result == EFS_OK && sync_calls == 0 && finish_calls == 0);
    image(path, 90000); shard_io_thread(&a); assert(a.result == EFS_ERR_IO && sync_calls == 1);
    fail_sync = 0; fail_stat = 1; shard_io_thread(&a); assert(a.result == EFS_ERR_IO); fail_stat = 0;
    image(path, 90000); fail_finish = 1; shard_io_thread(&a); assert(a.result == EFS_ERR_IO); fail_finish = 0;
    a.sync_write = 0;
    fail_close = 1; close_calls = 0; shard_io_thread(&a);
    assert(a.result == EFS_ERR_IO && close_calls == 1);
    assert(fcntl(last_fd, F_GETFD) == -1 && errno == EBADF);
    fail_close = 0; check(path, aligned, a.len, sum);
    shard_io_thread(&a); assert(a.result == EFS_OK);
    a.excl = 1; shard_io_thread(&a); assert(a.result == EFS_ERR_EXIST); check(path, aligned, a.len, sum);
    unlink(path); a.excl = 0; a.no_create = 1; shard_io_thread(&a);
    assert(a.result == EFS_ERR_NOT_FOUND && access(path, F_OK) == -1);
    /* Actual quota path: a full live dataset reports QUOTA; an existing
     * retry remains writable without a second charge. */
    struct efsd_server server = {0};
    pthread_mutex_init(&server.lock, NULL);
    server.id = 1; server.node_count = 1; server.nodes[0].id = 1;
    server.quota = 1000; server.nodes[0].used = 1000;
    struct efs_export ex = {.id = 1};
    efs_tls_chunk_gen = 1; /* every fragment object has a nonzero generation */
    efs_tls_path_hint = EFS_PATH_HINT_SKIP;
    assert(server_write_fragment_to_path(&server,&ex,42,0,0,aligned,10,path,NULL)==EFS_ERR_QUOTA);
    assert(access(path,F_OK)==-1 && server.nodes[0].used==1000);
    image(path,10); efs_tls_path_hint = -1;
    assert(server_write_fragment_to_path(&server,&ex,42,0,0,aligned,10,path,NULL)==EFS_OK);
    /* A NULL caller digest still produces a bound record over the stored,
     * zero-padded payload of the fixed fragment length. */
    {
        uint32_t fl = server_frag_len(&ex, 42);
        uint8_t *padded = calloc(1, fl); assert(padded); memcpy(padded, aligned, 10);
        struct efs_frag_ident id; uint8_t d[EFS_HASH_SIZE], rec[EFS_FRAG_RECORD_BYTES];
        fragment_ident(&ex, 42, 0, 0, fl, &id); efs_hash(padded, fl, d);
        assert(efs_frag_record_encode(&id, d, rec) == EFS_OK);
        assert(server.nodes[0].used==1000); check(path,padded,fl,rec); unlink(path);
        free(padded);
    }
    /* Generation 0 names no object: never written as an unbound file. */
    efs_tls_chunk_gen = 0; efs_tls_path_hint = -1;
    assert(server_write_fragment_to_path(&server,&ex,42,0,0,aligned,10,path,NULL)==EFS_ERR_INVAL);
    assert(access(path,F_OK)==-1); efs_tls_chunk_gen = 1;
    server.nodes[0].used=700; efs_tls_path_hint=EFS_PATH_HINT_SKIP;
    assert(server_write_fragment_to_path(&server,&ex,42,0,0,aligned,10,path,NULL)==EFS_OK);
    assert(server.nodes[0].used==710); unlink(path);
    pthread_mutex_destroy(&server.lock);
    free(aligned); rmdir(dir);
    puts("store overwrite: O_DIRECT only, exact aligned/unaligned/short/bounce tails, grow/shrink, errors and create guards PASS");
    return 0;
}
