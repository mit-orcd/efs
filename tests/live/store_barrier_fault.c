/* Test-only preload; marker selects fragment barriers and failed node IDs. */
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
    const char *node = getenv("EFS_TEST_STORE_NODE");
    char link[64], path[4096], selection[128] = {0};
    struct stat st;
    FILE *f = marker ? fopen(marker, "r") : NULL;
    if (f) {
        if (!fgets(selection, sizeof(selection), f)) selection[0] = 0;
        fclose(f);
        int selected = !strncmp(selection, "all", 3) || !strncmp(selection, "fail fragment", 13);
        if (node) {
            char token[32]; snprintf(token, sizeof(token), ",%s,", node);
            selected |= strstr(selection, token) != NULL;
        }
        int directory = strstr(selection, "directory") != NULL;
        if (selected && !fstat(fd, &st) &&
            (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode))) {
            snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
            ssize_t n = readlink(link, path, sizeof(path)-1);
            if (n > 0) {
                path[n] = 0;
                if (strstr(path, "/data/exports/")) {
                    fprintf(stderr, "test-store-barrier: refusing fragment %s fsync %s\n",
                            directory ? "directory" : "file", path);
                    errno = EIO; return -1;
                }
            }
        }
    }
    if (!real_fsync) { errno = ENOSYS; return -1; }
    return real_fsync(fd);
}
