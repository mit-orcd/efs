/* Scratch location for every EFS process (DI): $TMPDIR when set and
 * nonempty, else /tmp. Daemons, the client, tools and tests build every
 * scratch path, mkdtemp/mkstemp template and the stop-control endpoint
 * with efs_tmp_path(), so a run pointed at its own TMPDIR leaves nothing
 * in the shared /tmp. */
#ifndef EFS_TMPDIR_H
#define EFS_TMPDIR_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline const char *efs_tmpdir(void)
{
    const char *t = getenv("TMPDIR");
    return t && *t ? t : "/tmp";
}

/* buf = "$TMPDIR/<fmt...>", without a doubled '/'. Returns the full length,
 * as snprintf does: a value >= len means the path did not fit. */
static inline int efs_tmp_vpath(char *buf, size_t len, const char *fmt, va_list ap)
{
    const char *t = efs_tmpdir();
    int tl = (int)strlen(t);
    while (tl > 1 && t[tl - 1] == '/') tl--;
    int n = snprintf(buf, len, "%.*s/", tl, t);
    if (n < 0) return n;
    int m = vsnprintf(n < (int)len ? buf + n : NULL, n < (int)len ? len - (size_t)n : 0, fmt, ap);
    return m < 0 ? m : n + m;
}

static inline __attribute__((format(printf, 3, 4)))
int efs_tmp_path(char *buf, size_t len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = efs_tmp_vpath(buf, len, fmt, ap);
    va_end(ap);
    return n;
}

#endif
