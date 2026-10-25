/* Test-only observation of the automatic quota's real filesystem sample.
 * Return libc's result unchanged, including errno. Never preload in a real
 * deployment. The fresh n8 fixture has no fragment usage at startup. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/statvfs.h>
#include <unistd.h>

int statvfs(const char *path, struct statvfs *out)
{
    int (*real_fn)(const char *, struct statvfs *) = dlsym(RTLD_NEXT, "statvfs");
    if (!real_fn) { errno = ENOSYS; return -1; }
    int rc = real_fn(path, out), saved = errno;
    const char *trace = getenv("B_GEOMETRY_STATVFS_TRACE");
    if (!rc && trace) {
        int fd = open(trace, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
        if (fd >= 0) {
            unsigned long long unit = out->f_frsize ? out->f_frsize : out->f_bsize;
            dprintf(fd, "%s\t%llu\t%llu\n", path,
                    (unsigned long long)out->f_blocks * unit,
                    (unsigned long long)out->f_bavail * unit);
            close(fd);
        }
    }
    errno = saved;
    return rc;
}
