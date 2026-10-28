/* Diagnostic-only LD_PRELOAD observer. Never trims or changes malloc policy.
 * Only a newly launched efs-fuse starts the observer thread. The driver writes
 * the actual load-start epoch; snapshots follow at 20, 40 and 60 minutes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <malloc.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>

static char output[PATH_MAX];
/* Log application-visible mapping lifetimes without allocating memory. libc
 * internal mmap calls need not interpose; mallinfo2 covers malloc separately.
 * Direct syscall has mmap/munmap semantics on the assigned x86_64 Linux host.
 * This observer is diagnostic-only, never linked into the product. */
static int mapping_log = -1;
static unsigned mapping_errors;
static void mapping_event(const char *kind, void *address, size_t bytes, int flags,
                          void *caller)
{
    int saved = errno;
    if (mapping_log >= 0) {
        char line[192];
        int n = snprintf(line, sizeof(line), "%s %p %zu %x %p\n",
                         kind, address, bytes, flags, caller);
        if (n > 0 && (size_t)n < sizeof(line)) {
            ssize_t written = write(mapping_log, line, (size_t)n);
            if (written != n)
                __atomic_add_fetch(&mapping_errors, 1, __ATOMIC_RELAXED);
        }
    }
    errno = saved;
}
void *mmap(void *address, size_t bytes, int prot, int flags, int fd, off_t offset)
{
    void *p = (void *)syscall(SYS_mmap, address, bytes, prot, flags, fd, offset);
    if (p != MAP_FAILED)
        mapping_event("mmap", p, bytes, flags, __builtin_return_address(0));
    return p;
}
int munmap(void *address, size_t bytes)
{
    int rc = (int)syscall(SYS_munmap, address, bytes);
    if (!rc) mapping_event("munmap", address, bytes, 0, __builtin_return_address(0));
    return rc;
}
static void copy_proc_file(const char *name, const char *path)
{
    char input[64], buffer[16384];
    snprintf(input, sizeof(input), "/proc/self/%s", name);
    int in = open(input, O_RDONLY), out = open(path, O_WRONLY|O_CREAT|O_EXCL, 0600);
    if (in >= 0 && out >= 0) {
        ssize_t n;
        while ((n = read(in, buffer, sizeof(buffer))) > 0) {
            ssize_t pos = 0;
            while (pos < n) {
                ssize_t written = write(out, buffer + pos, (size_t)(n - pos));
                if (written <= 0) break;
                pos += written;
            }
            if (pos < n) break;
        }
    }
    if (in >= 0) close(in);
    if (out >= 0) close(out);
}

static double wall(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void *observe(void *unused) {
    (void)unused;
    char path[PATH_MAX + 128];
    double start = 0;
    snprintf(path, sizeof(path), "%s/load-start", output);
    while (start < 1e9) {
        FILE *f = fopen(path, "r");
        if (f) { if (fscanf(f, "%lf", &start) != 1) start = 0; fclose(f); }
        if (start < 1e9) sleep(1);
    }
    for (int minute = 20; minute <= 60; minute += 20) {
        while (wall() < start + minute * 60) sleep(1);
        snprintf(path, sizeof(path), "%s/%ld-%dm.smaps", output, (long)getpid(), minute);
        copy_proc_file("smaps", path);
        struct mallinfo2 m = mallinfo2();
        snprintf(path, sizeof(path), "%s/%ld-%dm.xml", output, (long)getpid(), minute);
        FILE *f = fopen(path, "w");
        if (f) { malloc_info(0, f); fclose(f); }
        snprintf(path, sizeof(path), "%s/%ld-%dm.json", output, (long)getpid(), minute);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "{\"pid\":%ld,\"elapsed_s\":%.3f,\"arena\":%zu,\"uordblks\":%zu,\"fordblks\":%zu,\"hblkhd\":%zu,\"keepcost\":%zu,\"mapping_errors\":%u}\n",
                    (long)getpid(), wall()-start, m.arena, m.uordblks, m.fordblks, m.hblkhd, m.keepcost,
                    __atomic_load_n(&mapping_errors, __ATOMIC_RELAXED));
            fclose(f);
        }
    }
    return NULL;
}
__attribute__((constructor)) static void start_observer(void) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe)-1);
    if (n < 0) return;
    exe[n] = 0;
    char *name = strrchr(exe, '/');
    if (!name || strcmp(name+1, "efs-fuse")) return;
    const char *dir = getenv("TASK_I_ALLOC_DIR");
    if (!dir || !*dir || strlen(dir) >= sizeof(output)) return;
    strcpy(output, dir);
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/%ld-mappings.log", output, (long)getpid());
    mapping_log = open(path, O_WRONLY|O_CREAT|O_EXCL|O_APPEND, 0600);
    snprintf(path, sizeof(path), "%s/%ld-maps.txt", output, (long)getpid());
    copy_proc_file("maps", path);
    pthread_t t;
    if (!pthread_create(&t, NULL, observe, NULL)) pthread_detach(t);
}
