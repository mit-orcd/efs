#ifndef EFS_RECORDER_START_H
#define EFS_RECORDER_START_H
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static inline int efs_recorder_directory(const char *path)
{
    if (mkdir(path, 0755) == 0) return 0;
    if (errno != EEXIST) return -1;
    struct stat st;
    if (stat(path, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}

/* Launch only: each caller owns its PID, start timing and shutdown policy.
 * In particular FUSE starts recorders after its daemon fork. */
static inline int efs_recorder_parent(const char *path)
{
    char dir[8192];
    if (!path || !*path) { errno = EINVAL; return -1; }
    if (strlen(path) >= sizeof(dir)) { errno = ENAMETOOLONG; return -1; }
    strcpy(dir, path);
    char *slash = strrchr(dir, '/');
    if (!slash) return 0;
    if (slash == dir) return 0;
    *slash = '\0';
    for (char *p = dir + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (efs_recorder_directory(dir)) return -1;
            *p = '/';
        }
    }
    if (*dir && efs_recorder_directory(dir)) return -1;
    return 0;
}

static inline pid_t efs_start_perf_recorder(pid_t target, const char *path)
{
    if (efs_recorder_parent(path)) return -1;
    pid_t pid = fork();
    if (pid < 0) { perror("fork perf"); return -1; }
    if (!pid) {
        char target_str[32];
        snprintf(target_str, sizeof(target_str), "%d", (int)target);
        execlp("perf", "perf", "record", "-g", "-F", "999", "-p",
               target_str, "-o", path, NULL);
        perror("exec perf");
        _exit(1);
    }
    return pid;
}

static inline pid_t efs_start_strace_recorder(pid_t target, const char *path)
{
    if (efs_recorder_parent(path)) return -1;
    pid_t pid = fork();
    if (pid < 0) { perror("fork strace"); return -1; }
    if (!pid) {
        char target_str[32];
        snprintf(target_str, sizeof(target_str), "%d", (int)target);
        const char *expr = getenv("EFS_STRACE_EXPR");
        const char *av[16] = {"strace", "-f", "-tt", "-T", "-qq",
                              "-o", path, "-p", target_str};
        int n = 9;
        if (expr && *expr) { av[n++] = "-e"; av[n++] = expr; }
        av[n] = NULL;
        execvp("strace", (char *const *)av);
        perror("exec strace");
        _exit(1);
    }
    return pid;
}
#endif
