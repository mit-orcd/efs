/* Test-only preload: fail regular fragment fsync, never metadata or directories. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int fsync(int fd)
{
    int (*real_fsync)(int) = dlsym(RTLD_NEXT, "fsync");
    const char *marker = getenv("EFS_TEST_STORE_BARRIER_FILE");
    char link[64], path[4096];
    struct stat st;
    if (marker && access(marker, F_OK) == 0 && !fstat(fd, &st) && S_ISREG(st.st_mode)) {
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path)-1);
        if (n > 0) {
            path[n] = 0;
            if (strstr(path, "/data/exports/")) {
                fprintf(stderr, "test-store-barrier: refusing fragment fsync %s\n", path);
                errno = EIO;
                return -1;
            }
        }
    }
    if (!real_fsync) { errno = ENOSYS; return -1; }
    return real_fsync(fd);
}
