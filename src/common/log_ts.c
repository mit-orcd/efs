/* Timestamps on every log line of a daemon.
 *
 * efsd and efs-fuse log with plain fprintf(stderr)/printf from hundreds
 * of sites. Instead of touching each one, stdout and stderr are replaced
 * by fopencookie() streams whose write function copies the bytes to the
 * original fd and inserts "YYYY-MM-DDTHH:MM:SS.mmmZ " at every line
 * start. The streams stay line-buffered, so a fprintf() line reaches the
 * cookie as one write under the FILE lock; the per-stream "at line start"
 * flag covers lines longer than the stdio buffer.
 *
 * Why (Oct 1 2026): 8 parallel dd + ecopy wedged fstor007's mount for
 * minutes behind REPORT retries; the client log had the retry lines but
 * no time, so the sequence (which REPORT, how long, before or after the
 * kill) had to be guessed. Every harness grep on these logs is a
 * substring match, so the prefix breaks none of them.
 *
 * EFS_LOG_TS=0 disables it (tools that pipe the output elsewhere). */
#include "efs/log_ts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/uio.h>
#include <errno.h>

struct ts_cookie {
    int fd;
    int at_line_start;
};

static size_t ts_stamp(char *buf, size_t len)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    int n = snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec,
                     ts.tv_nsec / 1000000L);
    return n > 0 ? (size_t)n : 0;
}

static int write_all(int fd, const struct iovec *iov, int n)
{
    while (n > 0) {
        ssize_t w = writev(fd, iov, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        /* Advance past what went out. */
        struct iovec *v = (struct iovec *)iov;
        while (n > 0 && (size_t)w >= v->iov_len) {
            w -= (ssize_t)v->iov_len;
            v++;
            n--;
        }
        if (n > 0) {
            v->iov_base = (char *)v->iov_base + w;
            v->iov_len -= (size_t)w;
            iov = v;
        }
    }
    return 0;
}

static ssize_t ts_write(void *cookie, const char *buf, size_t size)
{
    struct ts_cookie *c = cookie;
    char stamp[40];
    size_t slen = 0;
    size_t done = 0;
    /* One stamp per cookie write: a write is one logical line (or the
     * tail of a long one). Lines glued into one write all get the same
     * stamp, which is the truth to the millisecond anyway. */
    while (done < size) {
        const char *nl = memchr(buf + done, '\n', size - done);
        size_t seg = nl ? (size_t)(nl - (buf + done)) + 1 : size - done;
        struct iovec iov[2];
        int n = 0;
        if (c->at_line_start) {
            if (!slen)
                slen = ts_stamp(stamp, sizeof(stamp));
            iov[n].iov_base = stamp;
            iov[n].iov_len = slen;
            n++;
        }
        iov[n].iov_base = (char *)buf + done;
        iov[n].iov_len = seg;
        n++;
        if (write_all(c->fd, iov, n) != 0)
            return done ? (ssize_t)done : -1;
        c->at_line_start = nl != NULL;
        done += seg;
    }
    return (ssize_t)size;
}

static FILE *ts_wrap(int fd)
{
    struct ts_cookie *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fd = fd;
    c->at_line_start = 1;
    cookie_io_functions_t fns = {.write = ts_write};
    FILE *f = fopencookie(c, "w", fns);
    if (!f) {
        free(c);
        return NULL;
    }
    setvbuf(f, NULL, _IOLBF, 0);
    return f;
}

void efs_log_timestamps_install(void)
{
    const char *e = getenv("EFS_LOG_TS");
    if (e && strcmp(e, "0") == 0)
        return;
    fflush(stdout);
    fflush(stderr);
    FILE *out = ts_wrap(1);
    FILE *err = ts_wrap(2);
    if (out)
        stdout = out;
    if (err)
        stderr = err;
}
