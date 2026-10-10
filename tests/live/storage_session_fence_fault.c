/* Private fixture preload: fail only armed certificate-file durability. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int fsync(int fd)
{
    static __thread int (*real_sync)(int);
    static __thread int announced;
    if(!real_sync)real_sync=(int (*)(int))dlsym(RTLD_NEXT,"fsync");
    if(!real_sync){errno=EIO;return -1;}
    const char *armed=getenv("EFS_TEST_PUT_FENCE_FAULT_FILE");
    if(armed && access(armed,F_OK)==0){
        char proc[64],path[4096];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);
        ssize_t n=readlink(proc,path,sizeof(path)-1);
        if(n>0){
            path[n]=0;
            if(strstr(path,"/meta/put-session/") && strstr(path,".tmp.")){
                if(!announced){fprintf(stderr,"test-put-storage-fence: refusing certificate sync\n");announced=1;}
                errno=EIO;return -1;
            }
        }
    }
    return real_sync(fd);
}
