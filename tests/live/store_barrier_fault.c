/* Test-only preload: refuses the batched fragment barrier, syncfs(storage root),
 * on the nodes the marker file selects ("all" or ",<node>," tokens). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int syncfs(int fd)
{
    int (*real_syncfs)(int) = dlsym(RTLD_NEXT, "syncfs");
    const char *marker = getenv("EFS_TEST_STORE_BARRIER_FILE");
    const char *node = getenv("EFS_TEST_STORE_NODE");
    char selection[128] = {0};
    FILE *f = marker ? fopen(marker, "r") : NULL;
    if (f) {
        if (!fgets(selection, sizeof(selection), f)) selection[0] = 0;
        fclose(f);
        int selected = !strncmp(selection, "all", 3);
        if (node) {
            char token[32]; snprintf(token, sizeof(token), ",%s,", node);
            selected |= strstr(selection, token) != NULL;
        }
        /* Only a root with a durability record is a fragment root. */
        if (selected && !faccessat(fd, ".efs-durable-epoch", F_OK, 0)) {
            static int logged; /* the syncer retries without pause */
            if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
                fprintf(stderr, "test-store-barrier: refusing fragment syncfs node %s\n",
                        node ? node : "?");
            errno = EIO; return -1;
        }
    }
    if (!real_syncfs) { errno = ENOSYS; return -1; }
    return real_syncfs(fd);
}
