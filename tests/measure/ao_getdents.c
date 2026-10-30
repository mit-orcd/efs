/* AO measurement helper: actual getdents64, strict record framing, fresh fd. */
#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
struct dent64 {uint64_t ino;int64_t off;unsigned short reclen;unsigned char type;char name[];};
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    int fd=open(argv[1],O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0){perror("open");return 1;}
    _Alignas(uint64_t) char buf[4096];unsigned long count=0,calls=0;
    for(;;){long n=syscall(SYS_getdents64,fd,buf,sizeof(buf));calls++;
        if(n<0){perror("getdents64");close(fd);return 1;}if(!n)break;
        for(long pos=0;pos<n;){
            struct dent64 *d=(void *)(buf+pos);
            if(n-pos<(long)offsetof(struct dent64,name)+1 || d->reclen<offsetof(struct dent64,name)+1 ||
               d->reclen>n-pos || (d->reclen&7) || !memchr(d->name,0,d->reclen - offsetof(struct dent64,name))) {
                fprintf(stderr,"malformed getdents64 record\n");close(fd);return 1;
            }
            if(strcmp(d->name,".")&&strcmp(d->name,"..")){puts(d->name);count++;}
            pos+=d->reclen;
        }
    }
    if(close(fd)){perror("close");return 1;}
    fprintf(stderr,"ao-getdents names=%lu syscalls=%lu\n",count,calls);
    return fflush(stdout)||ferror(stdout)?1:0;
}
