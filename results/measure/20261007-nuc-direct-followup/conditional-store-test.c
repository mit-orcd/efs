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
static int test_open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if ((flags & O_SYNC) == O_SYNC) sync_opens++;
    return open(path, flags, mode);
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
#define EFS_BENCH_BUILD
#define open test_open
#define fsync test_fsync
#define fstat test_fstat
#define ftruncate test_ftruncate
#define write test_write
#define writev test_writev
#include "/tmp/efs-nuc-sync-study-20261007/src/server/store.c"
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
static void check(const char *path, const uint8_t *body, size_t len,
                  const uint8_t *sum, int direct)
{
    struct stat st; assert(stat(path, &st) == 0);
    size_t expected = direct ? len + (sum ? 4096 : 0) : len + (sum ? EFS_HASH_SIZE : 0);
    assert((size_t)st.st_size == expected);
    uint8_t *all = malloc(expected ? expected : 1); assert(all);
    int fd = open(path, O_RDONLY); assert(fd >= 0);
    assert(read(fd, all, expected) == (ssize_t)expected); close(fd);
    assert(memcmp(all, body, len) == 0);
    if (sum) {
        assert(memcmp(all + len, sum, EFS_HASH_SIZE) == 0);
        if (direct) for (size_t n = len + EFS_HASH_SIZE; n < expected; n++) assert(all[n] == 0);
    }
    free(all);
}
int main(void)
{
    char dir[] = "/tmp/efs-overwrite-XXXXXX"; assert(mkdtemp(dir));
    char path[1024]; snprintf(path, sizeof(path), "%s/body", dir);
    uint8_t *aligned; assert(posix_memalign((void **)&aligned, 4096, 65536) == 0);
    memset(aligned, 0x37, 65536); uint8_t sum[EFS_HASH_SIZE]; memset(sum, 0x42, sizeof(sum));
    struct shard_io_arg a = {0}; strcpy(a.path, path); a.buf = aligned;
    a.is_write = 1; a.sum = sum; a.len = 65536;
    image(path, 90000); shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, a.len, sum, 0);
    a.len = 1003; shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, a.len, sum, 0);
    a.len = 7003; shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, a.len, sum, 0);
    a.sum = NULL; a.len = 0; shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, 0, NULL, 0);
    a.direct = 1; a.sum = sum; a.len = 65536;
    image(path, 90000); shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, a.len, sum, 1);
    a.len = 4096; shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, a.len, sum, 1);
    a.sum = NULL; a.len = 8192; shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, a.len, NULL, 1);
    a.sum = sum; a.buf = aligned + 1; a.len = 4096; /* bounce path */
    shard_io_thread(&a); assert(a.result == EFS_OK); check(path, a.buf, a.len, sum, 1);
    a.direct = 0; a.buf = aligned; a.len = 1003; a.sum = sum;
    fail_finish = 1; finish_calls = 0; shard_io_thread(&a);
    assert(a.result == EFS_ERR_IO && finish_calls == 1);
    assert(fcntl(last_fd, F_GETFD) == -1 && errno == EBADF);
    fail_finish = 0; interrupt_finish = 1; finish_calls = 0; shard_io_thread(&a);
    assert(a.result == EFS_OK && finish_calls == 2); check(path, aligned, a.len, sum, 0);
    finish_calls = 0; stat_calls = 0; interrupt_stat = 1; shard_io_thread(&a);
    assert(a.result == EFS_OK && stat_calls == 2 && finish_calls == 0);
    fail_stat = 1; shard_io_thread(&a);
    assert(a.result == EFS_ERR_IO && finish_calls == 0);
    assert(fcntl(last_fd, F_GETFD) == -1 && errno == EBADF); fail_stat = 0;
    fail_write = 1; finish_calls = 0; shard_io_thread(&a);
    assert(a.result == EFS_ERR_IO && finish_calls == 0); fail_write = 0;
    check(path, aligned, a.len, sum, 0); /* failed before write: prior image survives */
    a.sync_write = 1; sync_opens = 0;
    shard_io_thread(&a); assert(a.result == EFS_OK && sync_opens == 1);
    check(path, aligned, a.len, sum, 0);
    fail_sync = 1; shard_io_thread(&a); assert(a.result == EFS_ERR_IO);
    fail_sync = 0; a.sync_write = 0;
    /* Conditional direct finalization: sync only a length change. */
    a.direct = 1; a.sync_write = 1; a.buf = aligned; a.len = 4096; a.sum = sum;
    image(path, 90000); sync_calls = 0;
    shard_io_thread(&a); assert(a.result == EFS_OK && sync_calls == 1);
    check(path, aligned, a.len, sum, 1);
    finish_calls = 0; sync_calls = 0; fail_sync = 1;
    shard_io_thread(&a); assert(a.result == EFS_OK && sync_calls == 0 && finish_calls == 0);
    image(path, 90000); shard_io_thread(&a); assert(a.result == EFS_ERR_IO && sync_calls == 1);
    fail_sync = 0; fail_stat = 1; shard_io_thread(&a); assert(a.result == EFS_ERR_IO); fail_stat = 0;
    image(path, 90000); fail_finish = 1; shard_io_thread(&a); assert(a.result == EFS_ERR_IO); fail_finish = 0;
    a.sum = NULL; a.len = 0; shard_io_thread(&a); assert(a.result == EFS_OK); uint8_t zero_page[4096] = {0}; check(path, zero_page, sizeof(zero_page), NULL, 1);
    a.direct = 0; a.sync_write = 0; a.sum = sum; a.len = 1003;
    shard_io_thread(&a); assert(a.result == EFS_OK); check(path, aligned, a.len, sum, 0);
    a.excl = 1; shard_io_thread(&a); assert(a.result == EFS_ERR_EXIST); check(path, aligned, a.len, sum, 0);
    unlink(path); a.excl = 0; a.no_create = 1; shard_io_thread(&a);
    assert(a.result == EFS_ERR_NOT_FOUND && access(path, F_OK) == -1);
    free(aligned); rmdir(dir);
    puts("store overwrite: exact buffered/direct/bounce tails, grow/shrink, zero, errors and create guards PASS");
    return 0;
}
