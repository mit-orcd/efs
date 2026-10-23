/* Scratch paths for unit tests (K25): every test directory or file lives
 * under $TMPDIR (default /tmp), never a hard-coded /tmp, so a run can be
 * pointed at a disk with room and leaves nothing behind.
 *
 *   char dir[EFS_TEST_PATH];
 *   efs_test_path(dir, sizeof(dir), "efs-thing-XXXXXX");
 *   assert(efs_test_mkdtemp(dir));        (or efs_test_mkstemp, or
 *                                          efs_test_own() for a fixed name)
 *
 * Owned paths are removed recursively when the creating process exits with
 * status 0. On a nonzero exit (glibc) or a crash/assert they are kept and
 * printed to stderr as "efs-test: kept <path>". Forked children never remove
 * or report them. */
#ifndef EFS_TEST_TMPDIR_H
#define EFS_TEST_TMPDIR_H

#include <ftw.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define EFS_TEST_PATH 1024
#define EFS_TEST_UNUSED __attribute__((unused))

static char **efs_test_owned;
static int efs_test_nowned;
static pid_t efs_test_owner;

static EFS_TEST_UNUSED const char *efs_test_tmpdir(void)
{
    const char *t = getenv("TMPDIR");
    return t && *t ? t : "/tmp";
}

/* buf = "$TMPDIR/<fmt...>", without a doubled '/'. */
static EFS_TEST_UNUSED char *efs_test_path(char *buf, size_t len, const char *fmt, ...)
{
    const char *t = efs_test_tmpdir();
    int tl = (int)strlen(t);
    while (tl > 1 && t[tl - 1] == '/') tl--;
    int n = snprintf(buf, len, "%.*s/", tl, t);
    va_list ap;
    va_start(ap, fmt);
    if (n > 0 && (size_t)n < len) vsnprintf(buf + n, len - (size_t)n, fmt, ap);
    va_end(ap);
    return buf;
}

static int efs_test_rm_one(const char *p, const struct stat *s, int flag, struct FTW *w)
{
    (void)s; (void)flag; (void)w;
    remove(p);
    return 0;
}

static void efs_test_report(void)
{
    for (int i = 0; i < efs_test_nowned; i++) {
        if (access(efs_test_owned[i], F_OK)) continue;
        /* write(2) only: also called from a signal handler. */
        ssize_t r = write(2, "efs-test: kept ", 15);
        r = write(2, efs_test_owned[i], strlen(efs_test_owned[i]));
        r = write(2, "\n", 1);
        (void)r;
    }
}

static void efs_test_finish(int status)
{
    if (getpid() != efs_test_owner) return;
    if (status) { efs_test_report(); return; }
    for (int i = 0; i < efs_test_nowned; i++)
        nftw(efs_test_owned[i], efs_test_rm_one, 32, FTW_DEPTH | FTW_PHYS);
}

#ifdef __GLIBC__
static void efs_test_on_exit(int status, void *arg) { (void)arg; efs_test_finish(status); }
#else
static void efs_test_at_exit(void) { efs_test_finish(0); }
#endif

static void efs_test_crash(int sig)
{
    if (getpid() == efs_test_owner) efs_test_report();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Register path (copied) for removal on success / report on failure. */
static EFS_TEST_UNUSED const char *efs_test_own(const char *path)
{
    if (!efs_test_owner) {
        efs_test_owner = getpid();
#ifdef __GLIBC__
        on_exit(efs_test_on_exit, NULL);
#else
        atexit(efs_test_at_exit);
#endif
        static const int sigs[] = { SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL };
        for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
            void (*old)(int) = signal(sigs[i], efs_test_crash);
            if (old != SIG_DFL) signal(sigs[i], old); /* keep a test's own handler */
        }
    }
    if (getpid() != efs_test_owner) return path;
    for (int i = 0; i < efs_test_nowned; i++)
        if (!strcmp(efs_test_owned[i], path)) return path;
    char **grown = realloc(efs_test_owned, sizeof(*grown) * (size_t)(efs_test_nowned + 1));
    char *copy = strdup(path);
    if (!grown || !copy) { free(copy); if (grown) efs_test_owned = grown; return path; }
    efs_test_owned = grown;
    efs_test_owned[efs_test_nowned++] = copy;
    return path;
}

static EFS_TEST_UNUSED char *efs_test_mkdtemp(char *tmpl)
{
    char *r = mkdtemp(tmpl);
    if (r) efs_test_own(r);
    return r;
}

static EFS_TEST_UNUSED int efs_test_mkstemp(char *tmpl)
{
    int fd = mkstemp(tmpl);
    if (fd >= 0) efs_test_own(tmpl);
    return fd;
}

#endif
