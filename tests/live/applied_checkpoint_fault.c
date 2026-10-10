/* Private-fixture interposer: retain old markers while KV/Raft sync normally. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
ssize_t pwrite(int fd,const void *buf,size_t count,off_t offset)
{
    static ssize_t (*real_pwrite)(int,const void*,size_t,off_t);
    if (!real_pwrite) real_pwrite=dlsym(RTLD_NEXT,"pwrite");
    const char *gate=getenv("EFS_TEST_APPLIED_FAULT");
    if (gate && !access(gate,F_OK)) {
        char link[64],path[PATH_MAX];
        snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
        ssize_t n=readlink(link,path,sizeof(path)-1);
        if (n>0) {
            path[n]=0;size_t len=strlen(path);
            const char *suffixes[]={"/mdraft/applied.0","/mdraft/applied.2"};
            for(unsigned i=0;i<2;i++) {
                size_t slen=strlen(suffixes[i]);
                if(len>=slen && !strcmp(path+len-slen,suffixes[i])) {
                    fprintf(stderr,"test-applied-marker: retaining checkpoint at %s\n",path);
                    errno=EIO;return -1;
                }
            }
        }
    }
    if(!real_pwrite){errno=ENOSYS;return -1;}
    return real_pwrite(fd,buf,count,offset);
}
