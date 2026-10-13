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
#include "efs/repair_job.h"
#include "efs/meta_cmd.h"
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

static int authority_rc,writer_status=EFS_INODE_RPC_OK,authority_calls;
void server_raft_host_submit(const uint8_t *p,uint32_t n,struct efs_msg_raft_mkfs_reply *out)
{assert(n==2+EFS_REPAIR_JOB_BYTES && (p[1]==EFS_MD_REQ_REPAIR_WRITE_AUTHORITY || p[1]==EFS_MD_REQ_REPAIR_SEAL_AUTHORITY));authority_calls++;memset(out,0,sizeof(*out));out->rc=authority_rc;}
void server_raft_host_writer_view(const struct efs_msg_inode_writer_view *q,struct efs_msg_inode_writer_view_reply *out)
{memset(out,0,sizeof(*out));out->status=writer_status;out->ino=q->ino;out->generation=q->generation;out->chunk_index=q->chunk_index;}
static void zero(const uint8_t *p){uint8_t empty[EFS_REPAIR_RECEIPT_BYTES]={0};assert(!memcmp(p,empty,sizeof(empty)));}

/* This is a physical-lock race test, not a distributed journal oracle. */
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int entered,release_gc,data_done,gc_rc,data_rc;
static struct efsd_server *target;
static uint8_t *request,output[EFS_REPAIR_RECEIPT_BYTES];
static uint32_t request_bytes;
int server_raft_host_repair_gc_ready(efs_ino_t ino,uint32_t ci,uint64_t object)
{(void)ino;(void)ci;(void)object;return EFS_OK;}
int server_raft_host_repair_gc_inode_local_ready(efs_ino_t ino)
{
 assert(ino==4097);
 pthread_mutex_lock(&gate);entered=1;pthread_cond_broadcast(&cv);
 while(!release_gc)pthread_cond_wait(&cv,&gate);
 pthread_mutex_unlock(&gate);return EFS_OK;
}
static void wait_flag(int *flag)
{
 struct timespec until;assert(!clock_gettime(CLOCK_REALTIME,&until));until.tv_sec+=5;
 while(!*flag)assert(!pthread_cond_timedwait(&cv,&gate,&until));
}
static void *gc_run(void *unused)
{(void)unused;gc_rc=server_repair_gc_inode(target,1,4097,7);return NULL;}
static void *data_run(void *unused)
{
 (void)unused;data_rc=server_repair_receive(target,request,request_bytes,0,output);
 pthread_mutex_lock(&gate);data_done=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);return NULL;
}
int main(void)
{
 for(int direct=0;direct<2;direct++){
  char root[]="/tmp/efs-repair-inode-race-XXXXXX";assert(mkdtemp(root));
  /* Keep each server identity allocated: inode-death TLS caches key on it. */
  target=calloc(1,sizeof(*target));assert(target);pthread_mutex_init(&target->lock,NULL);pthread_cond_init(&target->export_idle_cv,NULL);
  target->repair_experiment=1;target->id=1;target->node_count=1;target->nodes[0].id=1;target->storage_path_count=1;target->direct_io=direct;strcpy(target->storage_paths[0],root);
  target->export_count=1;target->exports[0].id=1;target->exports[0].chunk_size=EFS_MIN_CHUNK_SIZE;
  uint32_t len=EFS_MIN_CHUNK_SIZE/2;target->quota=len;
  struct efs_repair_job j={.ino=4097,.inode_gen=7,.object=101,.fragment_len=len,.epoch=1,.owner=1,.nodes={1,2,3},.state=EFS_REPAIR_ADMITTED};
  j.uuid[0]=1;assert(!server_repair_store_initialize(target,0,j.target_store));
  request_bytes=4+EFS_REPAIR_JOB_BYTES+len;request=calloc(1,request_bytes);assert(request);request[3]=1;memset(request+4+EFS_REPAIR_JOB_BYTES,77,len);efs_hash(request+4+EFS_REPAIR_JOB_BYTES,len,j.sums[0]);
  for(unsigned i=1;i<3;i++)memset(j.sums[i],i,32);
  assert(!efs_repair_job_encode(&j,request+4));
  entered=release_gc=data_done=0;authority_calls=0;
  pthread_t gc,data;assert(!pthread_create(&gc,NULL,gc_run,NULL));
  pthread_mutex_lock(&gate);wait_flag(&entered);pthread_mutex_unlock(&gate);
  /* The callback runs under inode WR. A session stripe holder waiting for
   * inode RD reproduces the ordinary PUT side of the manager's lock cycle. */
  struct efs_put_storage_guard session={0};uint8_t uuid[16]={9};assert(!server_put_session_begin(target,uuid,1,&session));
  assert(pthread_rwlock_tryrdlock(inode_gc_lock(1,4097))!=0);
  /* Another file in the same numerical shard must not inherit this GC
   * writer's exclusion. Previously every local-directory file collided. */
  pthread_rwlock_t *other=inode_gc_lock(1,8193);
  assert(other!=inode_gc_lock(1,4097));
  assert(!pthread_rwlock_tryrdlock(other));
  pthread_rwlock_unlock(other);
  server_put_session_end(&session);
  assert(!pthread_create(&data,NULL,data_run,NULL));
  pthread_mutex_lock(&gate);release_gc=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);
  assert(!pthread_join(gc,NULL));assert(!pthread_join(data,NULL));
  assert(!gc_rc);assert(data_rc==EFS_ERR_STALE);zero(output);assert(!target->nodes[0].used);
  assert(server_repair_receive(target,request,request_bytes,0,output)==EFS_ERR_STALE);zero(output);
  assert(!server_repair_receive(target,request,4+EFS_REPAIR_JOB_BYTES,1,output));
  assert(!efs_repair_receipt_verify(&j,EFS_REPAIR_RECEIPT_SEAL,output,sizeof(output)));
  free(request);
 }
 puts("physical inode GC guard buffered/direct: late DATA cannot resurrect or ACK; actual durable death and seal PASS; authority adapters, not quorum/debt-cleanup acceptance");return 0;
}
