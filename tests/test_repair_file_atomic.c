/* Actual production file repair primitive; no distributed repair claim. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
static int fail_file,fail_published_dir,published;
static int repair_sync(int fd){struct stat st;assert(!fstat(fd,&st));if((S_ISREG(st.st_mode)&&fail_file)||(S_ISDIR(st.st_mode)&&published&&fail_published_dir)){errno=EIO;return -1;}return fsync(fd);}
static int repair_rename(const char *a,const char *b){int rc=rename(a,b);if(!rc)published=1;return rc;}
#define fsync repair_sync
#define rename repair_rename
#include "../src/server/store.c"
#undef fsync
#undef rename
static void verify(struct efsd_server *s,struct efs_export *ex,uint8_t *expected,uint32_t len)
{
 uint8_t *got=malloc(len),sum[32],h[32];assert(got);uint32_t n=len;int present=0;
 efs_tls_chunk_gen=101;assert(!server_read_fragment_with_sum(s,ex,4097,0,0,got,&n,sum,&present));efs_tls_chunk_gen=0;
 assert(n==len&&present&&!memcmp(got,expected,len));efs_hash(got,n,h);assert(!memcmp(h,sum,32));free(got);
}
int main(void)
{
 char root[]="/tmp/efs-repair-file-XXXXXX";assert(mkdtemp(root));
 struct efsd_server *s=calloc(1,sizeof(*s));assert(s);pthread_mutex_init(&s->lock,NULL);
 s->id=1;s->node_count=1;s->nodes[0].id=1;s->storage_path_count=1;strcpy(s->storage_paths[0],root);
 struct efs_export ex={.id=1,.chunk_size=EFS_MIN_CHUNK_SIZE};uint32_t len=server_frag_len(&ex,4097);
 uint8_t *data=malloc(len),sum[32];assert(data);memset(data,77,len);efs_hash(data,len,sum);
 ex.chunk_size=0;assert(server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum)==EFS_ERR_INVAL);ex.chunk_size=EFS_MIN_CHUNK_SIZE;
 assert(server_repair_file_atomic(s,&ex,4097,0,3,101,data,len,sum)==EFS_ERR_INVAL);
 s->storage_engine=1;assert(server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum)==EFS_ERR_INVAL);s->storage_engine=0;
 for(int direct=0;direct<2;direct++){
  s->direct_io=direct;s->quota=len;s->nodes[0].used=0;
  sum[0]^=1;assert(server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum)==EFS_ERR_CHECKSUM);sum[0]^=1;
  fail_file=1;published=0;assert(server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum)==EFS_ERR_IO);assert(!s->nodes[0].used&&!published);fail_file=0;
  assert(!server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum));assert(s->nodes[0].used==len);verify(s,&ex,data,len);
  efs_tls_chunk_gen=101;char path[8192];server_fragment_path(s,&ex,4097,0,0,path,sizeof(path));efs_tls_chunk_gen=0;
  int fd=open(path,O_WRONLY);assert(fd>=0);uint8_t bad=0;assert(pwrite(fd,&bad,1,0)==1&&!close(fd));
  fail_file=1;published=0;assert(server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum)==EFS_ERR_IO);assert(!published&&s->nodes[0].used==len);fail_file=0;
  fd=open(path,O_RDONLY);assert(fd>=0);uint8_t old=1;assert(read(fd,&old,1)==1&&!close(fd)&&old==0);
  fail_published_dir=1;published=0;assert(server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum)==EFS_ERR_IO);assert(published&&s->nodes[0].used==len);fail_published_dir=0;published=0;
  assert(!server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum));assert(s->nodes[0].used==len);verify(s,&ex,data,len);
  pid_t child=fork();assert(child>=0);if(!child){verify(s,&ex,data,len);_exit(0);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
  assert(server_repair_file_atomic(s,&ex,4097,1,0,102,data,len,sum)==EFS_ERR_QUOTA);
  assert(!unlink(path));
 }
 free(data);free(s);puts("ordinary single-root atomic repair: buffered/direct, actual checksum, pre-rename EIO preserves old bytes, post-rename EIO unacked, quota/retry/cold read PASS");return 0;
}
