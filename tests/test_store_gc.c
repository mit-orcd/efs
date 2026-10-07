/* Real fragment files, two roots, stored-format changes and syscall failures. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
static int fault;
static int gc_fsync(int fd){if(fault==5){errno=EIO;return -1;}return fsync(fd);}
static int gc_open(const char *p,int f,...){int m=0;if(f&O_CREAT){va_list a;va_start(a,f);m=va_arg(a,int);va_end(a);}if(fault==1&&f==O_RDONLY){errno=EACCES;return -1;}return open(p,f,m);}
static ssize_t gc_pread(int f,void*b,size_t n,off_t o){if(fault==2){errno=EIO;return -1;}if(fault==3)return 1;return pread(f,b,n,o);}
static int gc_unlink(const char*p){if(fault==4){errno=EACCES;return -1;}return unlink(p);}
#define EFS_BENCH_BUILD
#define open gc_open
#define pread gc_pread
#define unlink gc_unlink
#define fsync gc_fsync
#include "../src/server/store.c"
#undef open
#undef pread
#undef unlink
#undef fsync
int main(void){
 char a[]="/tmp/efs-gc-a-XXXXXX",b[]="/tmp/efs-gc-b-XXXXXX";assert(mkdtemp(a)&&mkdtemp(b));
 struct efsd_server s={.id=1,.node_count=1,.storage_path_count=2,.quota=1ull<<30};
 pthread_mutex_init(&s.lock,NULL);s.nodes[0].id=1;strcpy(s.storage_paths[0],a);strcpy(s.storage_paths[1],b);strcpy(s.storage_path,a);
 struct efs_export ex={.id=1,.chunk_size=EFS_DEFAULT_CHUNK_SIZE};
 uint8_t *body;assert(!posix_memalign((void**)&body,4096,EFS_FRAGMENT_SIZE));memset(body,0x61,EFS_FRAGMENT_SIZE);
 uint8_t sum[EFS_HASH_SIZE],wrong[EFS_HASH_SIZE];memset(sum,0x62,sizeof(sum));memset(wrong,0x63,sizeof(wrong));
 for(int direct=0;direct<=1;direct++){
  s.direct_io=direct;ex.chunk_size=EFS_DEFAULT_CHUNK_SIZE;
  efs_tls_chunk_gen=100+direct;
  char paths[2][8192];
  for(int root=0;root<2;root++){efs_tls_write_root=root;assert(!server_write_fragment_with_sum_sync(&s,&ex,42,0,0,body,EFS_FRAGMENT_SIZE,sum));fragment_path_at(&s,root,&ex,42,0,0,paths[root],sizeof(paths[root]));}
  assert(s.nodes[0].used==2*EFS_FRAGMENT_SIZE);
  assert(server_compute_local_usage(&s)==2*EFS_FRAGMENT_SIZE);
  server_usage_save(&s); /* deliberately stale after the subsequent deletes */
  s.direct_io=!direct;ex.chunk_size=EFS_MAX_CHUNK_SIZE; /* restart with no learned geometry */
  assert(server_delete_fragment_if_sum(&s,&ex,42,0,0,wrong)==EFS_ERR_EXIST);
  for(int f=1;f<=4;f++){fault=f;assert(server_delete_fragment_if_sum(&s,&ex,42,0,0,sum)==EFS_ERR_IO);fault=0;assert(!access(paths[0],F_OK)&&!access(paths[1],F_OK));assert(s.nodes[0].used==2*EFS_FRAGMENT_SIZE);}
  uint64_t removed=s.gc_removed_fragments;
  fault=5;assert(server_delete_fragment_if_sum(&s,&ex,42,0,0,sum)==EFS_ERR_IO);fault=0;
  assert(s.gc_removed_fragments==removed+1); /* unlink happened, ACK must wait for sync */
  fault=5;assert(server_delete_fragment_if_sum(&s,&ex,42,0,0,sum)==EFS_ERR_IO);fault=0;
  assert(s.gc_removed_fragments==removed+1); /* absent-file retry still syncs */
  assert(!server_delete_fragment_if_sum(&s,&ex,42,0,0,sum));assert(s.gc_removed_fragments==removed+2&&!s.nodes[0].used);
  assert(access(paths[0],F_OK)&&access(paths[1],F_OK));assert(!server_compute_local_usage(&s));
  assert(!server_delete_fragment_if_sum(&s,&ex,42,0,0,sum)&&s.gc_removed_fragments==removed+2);
  s.nodes[0].used=123456;server_init_local_usage(&s);
  assert(s.nodes[0].used==0); /* crash snapshot cannot resurrect quota charges */
  struct efs_msg_gc_status_reply status;server_gc_status(&s,&status);
  assert(status.version==2&&status.removed_fragments==removed+2&&status.delete_errors>0);
  assert(status.last_delete_age_ms<10000);
  assert(status.io[0].ops && status.io[0].bytes && status.io[0].errors);
  assert(status.io[1].ops && !status.io[1].bytes && status.io[1].errors);
  assert(status.io[2].ops && !status.io[2].bytes && status.io[2].errors);
  s.gc_pass_start_us=gc_clock_us();s.gc_last_pass_us=s.gc_pass_start_us;
  s.gc_first_seen_us[0]=s.gc_pass_start_us;server_gc_status(&s,&status);
  assert(status.pass_elapsed_ms<10000 && status.last_pass_age_ms<10000);
  assert(status.first_seen_age_ms[0]<10000);
  s.gc_last_pass_us=gc_clock_us()+1000;server_gc_status(&s,&status);
  assert(status.last_pass_age_ms==0); /* concurrent newer clock sample */
 }
 assert(s.gc_removed_fragments==4&&s.gc_removed_payload_bytes==4*EFS_FRAGMENT_SIZE);
 assert(server_delete_fragment_if_sum(NULL,&ex,42,0,0,sum)==EFS_ERR_INVAL);
 /* Lost ACK: retry may be scheduled on another root, but probe must
  * find the original body and charge exactly once. */
 s.direct_io=0;ex.chunk_size=EFS_DEFAULT_CHUNK_SIZE;
 efs_tls_chunk_gen=777;efs_tls_write_root=0;efs_tls_path_hint=EFS_PATH_HINT_SKIP;
 assert(!server_write_fragment_with_sum_sync(&s,&ex,777,0,0,body,EFS_FRAGMENT_SIZE,sum));
 assert(s.nodes[0].used==EFS_FRAGMENT_SIZE);
 efs_tls_write_root=1;efs_tls_path_hint=-1;
 int retry_root=server_find_fragment_root(&s,&ex,777,0,0);assert(retry_root==0);
 efs_tls_write_root=retry_root;
 assert(!server_write_fragment_with_sum_sync(&s,&ex,777,0,0,body,EFS_FRAGMENT_SIZE,sum));
 assert(s.nodes[0].used==EFS_FRAGMENT_SIZE&&server_compute_local_usage(&s)==EFS_FRAGMENT_SIZE);
 char duplicate[8192];fragment_path_at(&s,1,&ex,777,0,0,duplicate,sizeof(duplicate));assert(access(duplicate,F_OK));
 assert(server_gc_inode(&s,1,777,99)==EFS_OK&&s.nodes[0].used==0);
 efs_tls_path_hint=-1;
 s.direct_io=0;ex.chunk_size=EFS_DEFAULT_CHUNK_SIZE;efs_tls_chunk_gen=200;
 for(int root=0;root<2;root++)for(unsigned ci=0;ci<70;ci++){
  efs_tls_write_root=root;
  assert(!server_write_fragment_with_sum_sync(&s,&ex,84,ci,0,body,EFS_FRAGMENT_SIZE,sum));
 }
 assert(server_gc_inode(&s,1,84,9)==EFS_ERR_BUSY); /* bounded physical orphan pass */
 assert(server_write_fragment_with_sum_sync(&s,&ex,84,71,0,body,EFS_FRAGMENT_SIZE,sum)==EFS_ERR_STALE);
 assert(server_gc_inode(&s,1,84,9)==EFS_OK && s.nodes[0].used==0);
 assert(server_gc_inode(&s,1,84,9)==EFS_OK);
 assert(server_gc_inode(&s,1,84,9)==EFS_OK);
 efs_tls_write_root=0;
 assert(!server_write_fragment_with_sum_sync(&s,&ex,4180,0,0,body,EFS_FRAGMENT_SIZE,sum));
 assert(server_gc_inode(&s,1,4180,10)==EFS_OK); /* adjacent bitmap bit preserves old fence */
 server_init_local_usage(&s);assert(s.nodes[0].used==0);
 assert(server_write_fragment_with_sum_sync(&s,&ex,84,71,0,body,EFS_FRAGMENT_SIZE,sum)==EFS_ERR_STALE);
 /* A failed death-fence sync preserves physical bytes for a durable retry. */
 efs_tls_write_root=0;
 assert(!server_write_fragment_with_sum_sync(&s,&ex,126,0,0,body,EFS_FRAGMENT_SIZE,sum));
 fault=5;assert(server_gc_inode(&s,1,126,11)==EFS_ERR_IO);fault=0;
 assert(s.nodes[0].used==EFS_FRAGMENT_SIZE);
 assert(server_gc_inode(&s,1,126,11)==EFS_OK && !s.nodes[0].used);
 assert(server_gc_inode(&s,1,42,7)==EFS_OK);
 /* Empty sharded path scaffolding must not accumulate after file deletion. */
 for(unsigned root=0;root<2;root++) {
  char path[8192];snprintf(path,sizeof(path),"%s/data/exports/1",s.storage_paths[root]);
  DIR *d=opendir(path);assert(d);struct dirent *entry;
  while((entry=readdir(d))) assert(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."));
  assert(!closedir(d));
 }
 /* Corrupt fence pages fail PUT closed, including an adjacent live inode. */
 char fence[8192];inode_death_path(&s,1,84,fence);
 int f=open(fence,O_WRONLY);assert(f>=0);uint8_t corrupt=255;
 assert(pwrite(f,&corrupt,1,0)==1 && !close(f));
 __atomic_fetch_add(&inode_gc_epoch,1,__ATOMIC_RELEASE);
 assert(server_write_fragment_with_sum_sync(&s,&ex,8276,0,0,body,EFS_FRAGMENT_SIZE,sum)==EFS_ERR_IO);
 assert(!s.nodes[0].used);
 free(body);printf("store GC: two roots, buffered/direct format switch, quota recount, checksum protection, syscall failures and idempotent deletion PASS\n");return 0;
}
