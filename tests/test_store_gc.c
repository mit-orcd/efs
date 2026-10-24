/* Real fragment files, two roots, stored-format changes and syscall failures. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
static int fault;
static int gc_fsync(int fd){if(fault==5){errno=EIO;return -1;}return fsync(fd);}
static int gc_syncfs(int fd){if(fault==5){errno=EIO;return -1;}return syncfs(fd);}
static int gc_open(const char *p,int f,...){int m=0;if(f&O_CREAT){va_list a;va_start(a,f);m=va_arg(a,int);va_end(a);}if(fault==1&&(f&O_ACCMODE)==O_RDONLY&&(f&O_DIRECT)){errno=EACCES;return -1;}return open(p,f,m);}
static ssize_t gc_pread(int f,void*b,size_t n,off_t o){if(fault==2){errno=EIO;return -1;}if(fault==3)return 1;return pread(f,b,n,o);}
static int gc_unlink(const char*p){if(fault==4){errno=EACCES;return -1;}return unlink(p);}
#define EFS_BENCH_BUILD
#define open gc_open
#define pread gc_pread
#define unlink gc_unlink
#define fsync gc_fsync
#define syncfs gc_syncfs
#include "../src/server/store.c"
#include "../src/server/reclaim.c"
#include "../src/server/segment_store.c"
#include "test_tmpdir.h"
#undef open
#undef pread
#undef unlink
#undef fsync
#undef syncfs
/* No Raft host exists in this physical-store fixture. */
#if EFS_BUILD_EXPIRY
void server_raft_host_expiry_status(struct efs_expiry_status *out){memset(out,0,sizeof(*out));}
#endif
int server_raft_host_active(void){return 0;}
int server_raft_host_inode_reaped(efs_ino_t ino,int *dead){(void)ino;*dead=0;return EFS_ERR_NOT_PRIMARY;}
/* The reclaim deleter's two durable steps: death bit, then whole-inode delete. */
/* The startup inventory efsd.c runs: the local counter is the full scan,
 * never the advisory usage.bin snapshot. */
static void startup_inventory(struct efsd_server *s)
{
    uint64_t used = server_compute_local_usage(s);
    assert(used != UINT64_MAX);
    pthread_mutex_lock(&s->lock);
    struct efs_node *local = server_local_node(s);
    assert(local);
    local->used = used;
    pthread_mutex_unlock(&s->lock);
}
static int gc_inode(struct efsd_server *s,efs_export_id_t eid,efs_ino_t ino){
 int rc=server_gc_death_set_many(s,eid,&ino,1);
 return rc!=EFS_OK?rc:server_gc_inode_delete(s,eid,ino,NULL,1);
}

/* I25 fixture helpers: whole-file copy (an object swap) and a byte flip. */
static void copy_file(const char *from,const char *to)
{
 int a=open(from,O_RDONLY);assert(a>=0);struct stat st;assert(!fstat(a,&st));
 uint8_t *b=malloc((size_t)st.st_size);assert(b&&pread(a,b,(size_t)st.st_size,0)==st.st_size);close(a);
 int z=open(to,O_WRONLY|O_TRUNC);assert(z>=0&&pwrite(z,b,(size_t)st.st_size,0)==st.st_size);close(z);free(b);
}
static void flip_byte(const char *path,off_t at)
{
 int z=open(path,O_RDWR);assert(z>=0);uint8_t c;assert(pread(z,&c,1,at)==1);c^=0x5a;
 assert(pwrite(z,&c,1,at)==1);close(z);
}
/* I25: GET and GC act only on a file whose record names exactly the
 * requested object; payload rot, another identity under the name and the
 * previous (payload-only digest) tail are refused with distinct codes. */
static void test_identity(struct efsd_server *s,struct efs_export *ex,uint8_t *body)
{
 uint8_t good[EFS_HASH_SIZE],sum[EFS_HASH_SIZE];efs_hash(body,EFS_FRAGMENT_SIZE,good);
 uint8_t *out;assert(!posix_memalign((void**)&out,4096,EFS_FRAGMENT_SIZE));
 {
  efs_ino_t ino=9000;
  ex->chunk_size=EFS_DEFAULT_CHUNK_SIZE;
  efs_tls_chunk_gen=300;efs_tls_write_root=0;efs_tls_path_hint=-1;
  for(uint32_t ci=0;ci<4;ci++)
   assert(!server_write_fragment_with_sum_sync(s,ex,ino,ci,0,body,EFS_FRAGMENT_SIZE,good));
  char p[4][8192];for(int i=0;i<4;i++)fragment_path_at(s,0,ex,ino,(uint32_t)i,0,p[i],sizeof(p[i]));
  uint32_t len=EFS_FRAGMENT_SIZE;int ok=0;
  assert(!server_read_fragment_with_sum(s,ex,ino,0,0,out,&len,sum,&ok));
  assert(ok==EFS_STORE_SUM_PRESENT&&len==EFS_FRAGMENT_SIZE&&!memcmp(sum,good,sizeof(sum))&&!memcmp(out,body,len));
  uint64_t refused=server_fragment_refusals();
  /* Object swap: chunk 1's complete, self-consistent file under chunk 0's
   * name. Same payload bytes and digest: only identity can tell. */
  copy_file(p[1],p[0]);len=EFS_FRAGMENT_SIZE;
  assert(server_read_fragment_with_sum(s,ex,ino,0,0,out,&len,sum,&ok)==EFS_ERR_FRAG_IDENTITY&&!ok);
  assert(server_delete_fragment_if_sum(s,ex,ino,0,0,good)==EFS_ERR_FRAG_IDENTITY&&!access(p[0],F_OK));
  /* Another generation of the same name. */
  efs_tls_chunk_gen=400;len=EFS_FRAGMENT_SIZE;
  char g[8192];fragment_path_at(s,0,ex,ino,1,0,g,sizeof(g));
  assert(!rename(p[1],g)); /* gen-300 bytes now named generation 400 */
  assert(server_read_fragment_with_sum(s,ex,ino,1,0,out,&len,sum,&ok)==EFS_ERR_FRAG_IDENTITY);
  assert(!rename(g,p[1]));efs_tls_chunk_gen=300;
  /* Payload rot. */
  flip_byte(p[2],100);len=EFS_FRAGMENT_SIZE;
  assert(server_read_fragment_with_sum(s,ex,ino,2,0,out,&len,sum,&ok)==EFS_ERR_CHECKSUM);
  /* Identity bytes rotted on disk: bound digest. */
  flip_byte(p[3],EFS_FRAGMENT_SIZE+24);len=EFS_FRAGMENT_SIZE;
  assert(server_read_fragment_with_sum(s,ex,ino,3,0,out,&len,sum,&ok)==EFS_ERR_CHECKSUM);
  assert(server_delete_fragment_if_sum(s,ex,ino,3,0,good)==EFS_ERR_CHECKSUM&&!access(p[3],F_OK));
  /* The previous on-disk format: payload then a 4 KiB tail page starting
   * with a bare digest. */
  {
   int z=open(p[2],O_WRONLY|O_TRUNC);assert(z>=0);
   assert(pwrite(z,body,EFS_FRAGMENT_SIZE,0)==EFS_FRAGMENT_SIZE);
   uint8_t tail[4096]={0};memcpy(tail,good,EFS_HASH_SIZE);
   assert(pwrite(z,tail,sizeof(tail),EFS_FRAGMENT_SIZE)==(ssize_t)sizeof(tail));close(z);
   len=EFS_FRAGMENT_SIZE;int rc=server_read_fragment_with_sum(s,ex,ino,2,0,out,&len,sum,&ok);
   assert(rc==EFS_ERR_FRAG_FORMAT&&!ok);
   assert(server_delete_fragment_if_sum(s,ex,ino,2,0,good)==EFS_ERR_FRAG_FORMAT);
   assert(!access(p[2],F_OK));
  }
  /* D42: the removed buffered layout (payload || bare 128-byte record) is
   * not this build's format, even with a valid record: GET and GC refuse it. */
  {
   uint8_t rec[4096];int z=open(p[1],O_RDONLY);assert(z>=0);
   assert(pread(z,rec,sizeof(rec),EFS_FRAGMENT_SIZE)==(ssize_t)sizeof(rec));close(z);
   z=open(p[1],O_WRONLY|O_TRUNC);assert(z>=0);
   assert(pwrite(z,body,EFS_FRAGMENT_SIZE,0)==EFS_FRAGMENT_SIZE);
   assert(pwrite(z,rec,EFS_FRAG_RECORD_BYTES,EFS_FRAGMENT_SIZE)==EFS_FRAG_RECORD_BYTES);close(z);
   len=EFS_FRAGMENT_SIZE;
   assert(server_read_fragment_with_sum(s,ex,ino,1,0,out,&len,sum,&ok)!=EFS_OK&&!ok);
   assert(server_delete_fragment_if_sum(s,ex,ino,1,0,good)!=EFS_OK&&!access(p[1],F_OK));
  }
  assert(!unlink(p[1])); /* the refused file is left for an operator */
  /* D42 audit: an unclean stop leaves a torn file (record cut short, or
   * the payload itself). Refused on GET, never served short or padded. */
  for(uint32_t ci=4;ci<6;ci++){
   assert(!server_write_fragment_with_sum_sync(s,ex,ino,ci,0,body,EFS_FRAGMENT_SIZE,good));
   char t[8192];fragment_path_at(s,0,ex,ino,ci,0,t,sizeof(t));
   assert(!truncate(t,ci==4?EFS_FRAGMENT_SIZE+EFS_FRAG_RECORD_BYTES/2:EFS_FRAGMENT_SIZE/2));
   len=EFS_FRAGMENT_SIZE;
   assert(server_read_fragment_with_sum(s,ex,ino,ci,0,out,&len,sum,&ok)==EFS_ERR_CHECKSUM&&!ok);
   assert(!unlink(t));
  }
  assert(server_fragment_refusals()>=refused+8);
  assert(gc_inode(s,1,ino)==EFS_OK);
  startup_inventory(s);assert(!s->nodes[0].used);
 }
 free(out);
}
int main(int argc,char **argv){
 assert(argc==1 || (argc==2 && !strcmp(argv[1],"unlimited")));
 char a[EFS_TEST_PATH],b[EFS_TEST_PATH];efs_test_path(a,sizeof(a),"efs-gc-a-XXXXXX");efs_test_path(b,sizeof(b),"efs-gc-b-XXXXXX");assert(efs_test_mkdtemp(a)&&efs_test_mkdtemp(b));
 struct efsd_server s={.id=1,.node_count=1,.storage_path_count=2,.quota=argc==2?0:1ull<<30};
 pthread_mutex_init(&s.lock,NULL);s.nodes[0].id=1;strcpy(s.storage_paths[0],a);strcpy(s.storage_paths[1],b);strcpy(s.storage_path,a);
 struct efs_export ex={.id=1,.chunk_size=EFS_DEFAULT_CHUNK_SIZE};
 uint8_t *body;assert(!posix_memalign((void**)&body,4096,EFS_FRAGMENT_SIZE));memset(body,0x61,EFS_FRAGMENT_SIZE);
 uint8_t sum[EFS_HASH_SIZE],wrong[EFS_HASH_SIZE];memset(sum,0x62,sizeof(sum));memset(wrong,0x63,sizeof(wrong));
 for(int pass=0;pass<=1;pass++){
  ex.chunk_size=EFS_DEFAULT_CHUNK_SIZE;
  efs_tls_chunk_gen=100+pass;
  char paths[2][8192];
  for(int root=0;root<2;root++){efs_tls_write_root=root;assert(!server_write_fragment_with_sum_sync(&s,&ex,42,0,0,body,EFS_FRAGMENT_SIZE,sum));fragment_path_at(&s,root,&ex,42,0,0,paths[root],sizeof(paths[root]));}
  assert(s.nodes[0].used==2*EFS_FRAGMENT_SIZE);
  assert(server_compute_local_usage(&s)==2*EFS_FRAGMENT_SIZE);
  server_usage_save(&s); /* deliberately stale after the subsequent deletes */
  ex.chunk_size=EFS_MAX_CHUNK_SIZE; /* restart with no learned geometry */
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
  s.nodes[0].used=123456;startup_inventory(&s);
  assert(s.nodes[0].used==0); /* crash snapshot cannot resurrect quota charges */
  struct efs_msg_gc_status_reply status;server_gc_status(&s,&status);
  assert(status.version==5&&status.removed_fragments==removed+2&&status.delete_errors>0);
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
 ex.chunk_size=EFS_DEFAULT_CHUNK_SIZE;
 efs_tls_chunk_gen=777;efs_tls_write_root=0;efs_tls_path_hint=EFS_PATH_HINT_SKIP;
 assert(!server_write_fragment_with_sum_sync(&s,&ex,777,0,0,body,EFS_FRAGMENT_SIZE,sum));
 assert(s.nodes[0].used==EFS_FRAGMENT_SIZE);
 efs_tls_write_root=1;efs_tls_path_hint=-1;
 int retry_root=server_find_fragment_root(&s,&ex,777,0,0);assert(retry_root==0);
 efs_tls_write_root=retry_root;
 assert(!server_write_fragment_with_sum_sync(&s,&ex,777,0,0,body,EFS_FRAGMENT_SIZE,sum));
 assert(s.nodes[0].used==EFS_FRAGMENT_SIZE&&server_compute_local_usage(&s)==EFS_FRAGMENT_SIZE);
 char duplicate[8192];fragment_path_at(&s,1,&ex,777,0,0,duplicate,sizeof(duplicate));assert(access(duplicate,F_OK));
 assert(gc_inode(&s,1,777)==EFS_OK&&s.nodes[0].used==0);
 efs_tls_path_hint=-1;
 ex.chunk_size=EFS_DEFAULT_CHUNK_SIZE;efs_tls_chunk_gen=200;
 for(int root=0;root<2;root++)for(unsigned ci=0;ci<70;ci++){
  efs_tls_write_root=root;
  assert(!server_write_fragment_with_sum_sync(&s,&ex,84,ci,0,body,EFS_FRAGMENT_SIZE,sum));
 }
 { efs_ino_t dead=84; assert(server_gc_death_set_many(&s,1,&dead,1)==EFS_OK); }
 assert(server_write_fragment_with_sum_sync(&s,&ex,84,71,0,body,EFS_FRAGMENT_SIZE,sum)==EFS_ERR_STALE);
 assert(server_gc_inode_delete(&s,1,84,NULL,1)==EFS_OK && s.nodes[0].used==0); /* 140 files, beyond one 128-entry slice */
 assert(gc_inode(&s,1,84)==EFS_OK);
 assert(gc_inode(&s,1,84)==EFS_OK);
 efs_tls_write_root=0;
 assert(!server_write_fragment_with_sum_sync(&s,&ex,4180,0,0,body,EFS_FRAGMENT_SIZE,sum));
 assert(gc_inode(&s,1,4180)==EFS_OK); /* adjacent bitmap bit preserves old fence */
 startup_inventory(&s);assert(s.nodes[0].used==0);
 assert(server_write_fragment_with_sum_sync(&s,&ex,84,71,0,body,EFS_FRAGMENT_SIZE,sum)==EFS_ERR_STALE);
 /* A failed death-fence sync preserves physical bytes for a durable retry. */
 efs_tls_write_root=0;
 assert(!server_write_fragment_with_sum_sync(&s,&ex,126,0,0,body,EFS_FRAGMENT_SIZE,sum));
 fault=5;assert(gc_inode(&s,1,126)==EFS_ERR_IO);fault=0;
 assert(s.nodes[0].used==EFS_FRAGMENT_SIZE);
 assert(gc_inode(&s,1,126)==EFS_OK && !s.nodes[0].used);
 assert(gc_inode(&s,1,42)==EFS_OK);
 /* Empty sharded path scaffolding must not accumulate after file deletion. */
 for(unsigned root=0;root<2;root++) {
  char path[8192];snprintf(path,sizeof(path),"%s/data/exports/1",s.storage_paths[root]);
  DIR *d=opendir(path);assert(d);struct dirent *entry;
  while((entry=readdir(d))) assert(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."));
  assert(!closedir(d));
 }
 test_identity(&s,&ex,body);
 /* Corrupt fence pages fail PUT closed, including an adjacent live inode. */
 char fence[8192];inode_death_path(&s,1,84,fence);
 int f=open(fence,O_WRONLY);assert(f>=0);uint8_t corrupt=255;
 assert(pwrite(f,&corrupt,1,0)==1 && !close(f));
 __atomic_fetch_add(&inode_gc_epoch,1,__ATOMIC_RELEASE);
 assert(server_write_fragment_with_sum_sync(&s,&ex,8276,0,0,body,EFS_FRAGMENT_SIZE,sum)==EFS_ERR_IO);
 assert(!s.nodes[0].used);
 free(body);printf("store GC: two roots, O_DIRECT only (buffered layout refused), quota recount, checksum protection, I25 identity/format refusal, syscall failures and idempotent deletion PASS\n");return 0;
}
