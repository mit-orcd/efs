#ifndef EFS_BENCH_PERF_CONTROL_H
#define EFS_BENCH_PERF_CONTROL_H
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
static int perf_command(const char *command)
{
    const char *ctl = getenv("EFS_BENCH_PERF_CONTROL"), *ack = getenv("EFS_BENCH_PERF_ACK");
    if (!ctl && !ack) return 0;
    if (!ctl || !ack) { errno = EINVAL; return -1; }
    int fd = open(ctl, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    int af = open(ack, O_RDWR | O_CLOEXEC);
    if (af < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    char buf[32]; struct pollfd p = { .fd = af, .events = POLLIN };
    ssize_t written = write(fd, command, strlen(command));
    int rc = written == (ssize_t)strlen(command) ? 0 : -1;
    if (rc && written >= 0) errno = EIO;
    if (!rc) { int prc = poll(&p, 1, 5000); if (prc <= 0) { if (!prc) errno = ETIMEDOUT; rc = -1; } }
    if (!rc) { ssize_t n = read(af, buf, sizeof(buf)); if (n < 3 || memcmp(buf, "ack", 3)) { if (n >= 0) errno = EPROTO; rc = -1; } }
    int saved = errno;
    close(fd); close(af); errno = saved; return rc;
}
#endif
