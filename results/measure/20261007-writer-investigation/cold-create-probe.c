/* Isolated cold-create control. Both variants use the actual shard writer.
 * Legacy emulates its old O_TRUNC open and absence of the new fstat syscall. */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <time.h>
static int legacy;
static int cold_open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if (legacy && (flags & O_WRONLY)) flags |= O_TRUNC;
    return open(path, flags, mode);
}
static int cold_stat(int fd, struct stat *st)
{
    if (legacy) { memset(st, 0, sizeof(*st)); st->st_size = 65536 + 32; return 0; }
    return fstat(fd, st);
}
#define open cold_open
#define fstat cold_stat
#include "/tmp/efs-writer-study-20261007/src/server/store.c"
#undef open
#undef fstat
static pthread_barrier_t barrier;
struct worker { char root[1024]; unsigned slot; };
static void *run_worker(void *arg)
{
    struct worker *w = arg;
    uint8_t *buf = malloc(65536); assert(buf); memset(buf, 0x37, 65536);
    uint8_t sum[32]; memset(sum, 0x42, sizeof(sum));
    struct shard_io_arg a = {0}; a.buf = buf; a.sum = sum; a.len = 65536; a.is_write = 1;
    pthread_barrier_wait(&barrier);
    for (unsigned n = 0; n < 64; n++) {
        snprintf(a.path, sizeof(a.path), "%s/%u/%u", w->root, w->slot, n);
        shard_io_thread(&a); assert(a.result == EFS_OK);
    }
    free(buf); return NULL;
}
int main(int argc, char **argv)
{
    assert(argc == 3); legacy = atoi(argv[2]);
    pthread_t tids[16]; struct worker workers[16];
    pthread_barrier_init(&barrier, NULL, 17);
    for (unsigned i = 0; i < 16; i++) {
        snprintf(workers[i].root, sizeof(workers[i].root), "%s", argv[1]); workers[i].slot = i;
        char dir[1100]; snprintf(dir, sizeof(dir), "%s/%u", argv[1], i); assert(mkdir(dir, 0700) == 0);
        assert(pthread_create(&tids[i], NULL, run_worker, &workers[i]) == 0);
    }
    struct timespec a, z; clock_gettime(CLOCK_MONOTONIC, &a); pthread_barrier_wait(&barrier);
    for (unsigned i = 0; i < 16; i++) pthread_join(tids[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &z); double wall = z.tv_sec-a.tv_sec+(z.tv_nsec-a.tv_nsec)/1e9;
    printf("COLD legacy=%d ops=1024 bytes=67108864 wall_s=%.6f GiB_s=%.6f\n",legacy,wall,0.0625/wall);
    pthread_barrier_destroy(&barrier); return 0;
}
