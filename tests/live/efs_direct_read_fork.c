/* Read-only ORCD fork/direct-read diagnostic. Not a full capacity oracle.
 * cc -O2 -pthread efs_direct_read_fork.c -lcrypto -o /tmp/efs-read-fork
 * /tmp/efs-read-fork ROOT 0; EFS_DIAG_FORK=1 /tmp/efs-read-fork ROOT 0
 * ROOT is the 8-worker, 1,638,400-file W92 real-payload fixture.
 * Writes mismatch dumps only to /tmp, never the mounted fixture.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
static const char *root; static int prefill; static atomic_int stop; static atomic_ulong verified;
static void *worker(void *arg) {
 unsigned w=(unsigned)(uintptr_t)arg;
 for(unsigned off=0;off<10000 && !atomic_load(&stop);++off) {
  uint64_t index=204800ull*w+off;char path[4096];
  snprintf(path,sizeof(path),"%s/worker-%02u/dir-%06u/file-%09llu",root,w,off/1024,(unsigned long long)index);
  int fd=open(path,O_RDONLY);if(fd<0){perror(path);atomic_store(&stop,1);return NULL;}
  unsigned char input[25],seed[32];memcpy(input,"W92-real-payload:",17);
  for(unsigned k=0;k<8;++k) input[17+k]=(unsigned char)(index>>(56-8*k));SHA256(input,25,seed);
  unsigned char *held[4]={0};
  for(unsigned part=0;part<4;++part){
   unsigned char *allocation=malloc(65536+33);if(!allocation)abort();unsigned char *buf=allocation+32;held[part]=allocation;buf[65536]=0;if(prefill)memset(buf,0xaa,65536);
   ssize_t n=read(fd,buf,65536);int bad=-1;
   if(n!=65536)bad=0;else for(int k=0;k<65536;++k)if(buf[k]!=seed[k%32]){bad=k;break;}
   if(bad>=0){
    atomic_store(&stop,1);fprintf(stderr,"FAIL index=%llu part=%u bad=%d n=%zd prefill=%d errno=%d\n",(unsigned long long)index,part,bad,n,prefill,errno);
    char dump[256];snprintf(dump,sizeof(dump),"/tmp/native-mismatch-%llu-%u.bin",(unsigned long long)index,part);int out=open(dump,O_WRONLY|O_CREAT|O_EXCL,0600);if(out>=0){if(n>0)write(out,buf,n);close(out);}for(unsigned k=0;k<=part;++k)free(held[k]);close(fd);return NULL;
   }
  }
  for(unsigned k=0;k<4;++k)free(held[k]);
  close(fd);atomic_fetch_add(&verified,1);
 }
 return NULL;
}
static atomic_int done;
static void *forker(void *unused){(void)unused;while(!atomic_load(&done)&&!atomic_load(&stop)){pid_t p=fork();if(!p)_exit(0);if(p>0)waitpid(p,NULL,0);usleep(5000);}return NULL;}
int main(int argc,char **argv){if(argc<2)return 2;root=argv[1];prefill=argc>2?atoi(argv[2]):0;int do_fork=getenv("EFS_DIAG_FORK")!=NULL;pthread_t forks;if(do_fork)pthread_create(&forks,0,forker,0);pthread_t threads[8];for(unsigned i=0;i<8;++i)if(pthread_create(&threads[i],0,worker,(void*)(uintptr_t)i))abort();for(unsigned i=0;i<8;++i)pthread_join(threads[i],0);atomic_store(&done,1);if(do_fork)pthread_join(forks,0);printf("%s files=%lu prefill=%d\n",atomic_load(&stop)?"FAIL":"PASS",atomic_load(&verified),prefill);return atomic_load(&stop)?1:0;}
