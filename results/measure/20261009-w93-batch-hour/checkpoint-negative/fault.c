
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int fdatasync(int fd) {
    static int (*real_sync)(int);
    if (!real_sync) real_sync=dlsym(RTLD_NEXT,"fdatasync");
    const char *flag=getenv("TEST_KV_BARRIER_FILE");
    if (flag && access(flag,F_OK)==0) {
        char link[64],path[PATH_MAX];
        snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
        ssize_t n=readlink(link,path,sizeof(path)-1);
        if (n>0) {
            path[n]=0;
            if (strstr(path,"/mdraft/kv/wal")) { errno=EIO; return -1; }
        }
    }
    return real_sync(fd);
}
