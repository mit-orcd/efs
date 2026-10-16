/* Actual inactive production missing-role primitive; no distributed claim. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int fail_file,fail_published_dir,published,die_before_publish,partial_failure,writes;
static int repair_sync(int fd){struct stat st;assert(!fstat(fd,&st));if((S_ISREG(st.st_mode)&&fail_file)||(S_ISDIR(st.st_mode)&&published&&fail_published_dir)){errno=EIO;return -1;}int rc=fsync(fd);if(!rc&&S_ISREG(st.st_mode)&&die_before_publish)_exit(0);return rc;}
static int repair_link(int a,const char *b,int c,const char *d,int flags){int rc=linkat(a,b,c,d,flags);if(!rc)published=1;return rc;}
static ssize_t repair_write(int fd,const void *buf,size_t n,off_t off){if(partial_failure){if(writes++){errno=EIO;return -1;}if(n>4096)n=4096;}return pwrite(fd,buf,n,off);}
#define fsync repair_sync
#define linkat repair_link
#define pwrite repair_write
#include "../src/server/store.c"
#undef fsync
#undef linkat
#undef pwrite
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

 for(int direct=0;direct<2;direct++){
  s->direct_io=direct;s->quota=len;s->nodes[0].used=0;
  efs_tls_chunk_gen=101;char path[8192];server_fragment_path(s,&ex,4097,0,0,path,sizeof(path));efs_tls_chunk_gen=0;
  sum[0]^=1;assert(server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum)==EFS_ERR_CHECKSUM);sum[0]^=1;
  partial_failure=1;writes=0;assert(server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum)==EFS_ERR_IO);partial_failure=0;assert(!s->nodes[0].used&&access(path,F_OK)&&errno==ENOENT);
  fail_file=1;published=0;assert(server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum)==EFS_ERR_IO);assert(!s->nodes[0].used&&!published);fail_file=0;
  pid_t child=fork();assert(child>=0);if(!child){die_before_publish=1;server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum);_exit(4);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));assert(access(path,F_OK)&&errno==ENOENT);
  char parent[8192];strcpy(parent,path);*strrchr(parent,'/')=0;DIR *d=opendir(parent);assert(d);struct dirent *ent;unsigned files=0;while((ent=readdir(d)))if(strcmp(ent->d_name,".")&&strcmp(ent->d_name,".."))files++;closedir(d);assert(!files);
  fail_published_dir=1;published=0;assert(server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum)==EFS_ERR_IO);assert(published&&s->nodes[0].used==len);fail_published_dir=0;published=0;
  assert(!server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum));assert(s->nodes[0].used==len);verify(s,&ex,data,len);
  fail_file=1;assert(server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum)==EFS_ERR_IO);fail_file=0;
  int fd=open(path,O_WRONLY);assert(fd>=0);uint8_t bad=0;assert(pwrite(fd,&bad,1,0)==1&&!close(fd));
  assert(server_repair_file_atomic(s,&ex,4097,0,0,101,data,len,sum)==EFS_ERR_CHECKSUM);fd=open(path,O_RDONLY);assert(fd>=0);uint8_t old=1;assert(read(fd,&old,1)==1&&!close(fd)&&old==0);
  assert(server_repair_file_atomic(s,&ex,4097,1,0,102,data,len,sum)==EFS_ERR_QUOTA);assert(!unlink(path));
 }
 free(data);free(s);puts("inactive missing ordinary role O_TMPFILE: unprivileged buffered/direct publication, partial-write/sync EIO, abrupt pre-link death no named orphan, post-link EIO unacked, real hash retry/corrupt refusal/quota PASS");return 0;
}
