/* Test-only fsync interposer: refuse one selected private follower KV WAL. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int (*real_fdatasync)(int);
static int fired;
int fdatasync(int fd)
{
    if (!real_fdatasync) real_fdatasync=dlsym(RTLD_NEXT,"fdatasync");
    const char *gate=getenv("EFS_TEST_KV_WAL_FAULT"), *snapshot=getenv("EFS_TEST_KV_CHECKPOINT");
    char link[64],path[PATH_MAX];
    if (gate && snapshot && !access(gate,F_OK)) {
        snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
        ssize_t n=readlink(link,path,sizeof(path)-1);
        if(n>0) {
            path[n]=0;
            const char *suffix="/mdraft/kv/wal";size_t plen=strlen(path),slen=strlen(suffix);
            if(plen>=slen && !strcmp(path+plen-slen,suffix) && !__atomic_exchange_n(&fired,1,__ATOMIC_SEQ_CST)) {
                uint8_t saved[32]={0};path[plen-strlen("/kv/wal")]=0;
                for(unsigned i=0;i<2;i++) {
                    char marker[PATH_MAX+32];snprintf(marker,sizeof(marker),"%s/applied.%u",path,i*2);
                    int in=open(marker,O_RDONLY);
                    if(in>=0){ssize_t got=read(in,saved+16*i,16);if(got!=16)memset(saved+16*i,0,16);close(in);}
                }
                int out=open(snapshot,O_WRONLY|O_CREAT|O_EXCL,0600);
                if(out>=0){if(write(out,saved,sizeof(saved))!=(ssize_t)sizeof(saved))_exit(99);if(real_fdatasync)real_fdatasync(out);close(out);}
                fprintf(stderr,"test-kv-wal: refusing WAL fdatasync; captured previous applied checkpoints\n");
                errno=EIO;return -1;
            }
        }
    }
    if(!real_fdatasync){errno=ENOSYS;return -1;}
    return real_fdatasync(fd);
}
