#include "efs/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef EFS_VERSION
#define EFS_VERSION "unknown"
#endif
#ifndef EFS_GIT_BRANCH
#define EFS_GIT_BRANCH "unknown"
#endif
#ifndef EFS_BUILD_TIME
#define EFS_BUILD_TIME "unknown"
#endif

const char *efs_version_string(const char *prog, char *buf, size_t len)
{
    const char *base = strrchr(prog, '/');
    char host[64];

    if (base)
        prog = base + 1;
    if (gethostname(host, sizeof(host)) != 0)
        snprintf(host, sizeof(host), "unknown");
    host[sizeof(host) - 1] = '\0';
    if (strcmp(EFS_VERSION, "unknown") == 0)
        snprintf(buf, len, "%s (no git metadata, branch %s, built %s on %s)",
                 prog, EFS_GIT_BRANCH, EFS_BUILD_TIME, host);
    else
        snprintf(buf, len, "%s %s (branch %s, built %s on %s)",
                 prog, EFS_VERSION, EFS_GIT_BRANCH, EFS_BUILD_TIME, host);
    return buf;
}

void efs_version_check_argv(const char *prog, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            char buf[256];
            printf("%s\n", efs_version_string(prog, buf, sizeof(buf)));
            exit(0);
        }
    }
}
