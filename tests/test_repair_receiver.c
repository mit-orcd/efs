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
int main(void)
{
 for(int direct=0;direct<2;direct++){
  char root[]="/tmp/efs-repair-receiver-XXXXXX";assert(mkdtemp(root));
  struct efsd_server *s=calloc(1,sizeof(*s));assert(s);pthread_mutex_init(&s->lock,NULL);pthread_cond_init(&s->export_idle_cv,NULL);
  s->id=1;s->node_count=1;s->nodes[0].id=1;s->storage_path_count=1;s->direct_io=direct;strcpy(s->storage_paths[0],root);
  s->export_count=1;s->exports[0].id=1;s->exports[0].chunk_size=EFS_MIN_CHUNK_SIZE;
  uint32_t len=EFS_MIN_CHUNK_SIZE/2;s->quota=len;
  struct efs_repair_job j={.ino=4097,.inode_gen=7,.object=101,.fragment_len=len,.epoch=1,.owner=1,.nodes={1,2,3},.state=EFS_REPAIR_ADMITTED};
  j.uuid[0]=1;assert(!server_repair_store_initialize(s,0,j.target_store));
  uint8_t *body=calloc(1,4+EFS_REPAIR_JOB_BYTES+len),receipt[EFS_REPAIR_RECEIPT_BYTES];assert(body);body[3]=1;memset(body+4+EFS_REPAIR_JOB_BYTES,77,len);efs_hash(body+4+EFS_REPAIR_JOB_BYTES,len,j.sums[0]);
  for(unsigned i=1;i<3;i++)memset(j.sums[i],i,32);
  assert(!efs_repair_job_encode(&j,body+4));uint32_t bytes=4+EFS_REPAIR_JOB_BYTES+len;
  authority_rc=EFS_ERR_BUSY;assert(server_repair_receive(s,body,bytes,0,receipt)==EFS_ERR_BUSY);zero(receipt);assert(!s->nodes[0].used);authority_rc=0;
  writer_status=EFS_INODE_RPC_STALE;assert(server_repair_receive(s,body,bytes,0,receipt)==EFS_ERR_STALE);zero(receipt);writer_status=EFS_INODE_RPC_OK;
  body[bytes-1]^=1;int old_calls=authority_calls;assert(server_repair_receive(s,body,bytes,0,receipt)==EFS_ERR_CHECKSUM);zero(receipt);assert(authority_calls==old_calls);body[bytes-1]^=1;
  struct efs_repair_job wrong=j;wrong.target_store[0]^=1;assert(!efs_repair_job_encode(&wrong,body+4));assert(server_repair_receive(s,body,bytes,0,receipt)==EFS_ERR_STALE);zero(receipt);assert(!efs_repair_job_encode(&j,body+4));
  fail_file=1;assert(server_repair_receive(s,body,bytes,0,receipt)==EFS_ERR_IO);zero(receipt);assert(!s->nodes[0].used);fail_file=0;
  published=0;fail_published_dir=1;assert(server_repair_receive(s,body,bytes,0,receipt)==EFS_ERR_IO);zero(receipt);assert(s->nodes[0].used==len);fail_published_dir=0;
  assert(!server_repair_receive(s,body,bytes,0,receipt));assert(!efs_repair_receipt_verify(&j,EFS_REPAIR_RECEIPT_DATA,receipt,sizeof(receipt)));assert(s->nodes[0].used==len);
  uint8_t *got=malloc(len),sum[32];uint32_t n=len;int present=0;efs_tls_chunk_gen=j.object;assert(!server_read_fragment_with_sum(s,&s->exports[0],j.ino,0,0,got,&n,sum,&present));efs_tls_chunk_gen=0;
  assert(present&&n==len&&!memcmp(got,body+4+EFS_REPAIR_JOB_BYTES,len));efs_hash(got,n,sum);assert(!memcmp(sum,j.sums[0],32));free(got);
  authority_rc=EFS_ERR_BUSY;assert(server_repair_receive(s,body,4+EFS_REPAIR_JOB_BYTES,1,receipt)==EFS_ERR_BUSY);zero(receipt);authority_rc=0;
  assert(!server_repair_receive(s,body,4+EFS_REPAIR_JOB_BYTES,1,receipt));assert(!efs_repair_receipt_verify(&j,EFS_REPAIR_RECEIPT_SEAL,receipt,sizeof(receipt)));
  assert(server_repair_receive(s,body,bytes,0,receipt)==EFS_ERR_STALE);zero(receipt);
  struct efsd_server cold={.id=1,.storage_path_count=1};strcpy(cold.storage_paths[0],root);assert(!server_repair_store_matches(&cold,j.target_store));struct efs_put_storage_guard guard={0};assert(server_repair_storage_begin(&cold,j.uuid,j.epoch,&guard)==EFS_ERR_STALE);
  free(body);free(s);
 }
 puts("inactive actual receiver buffered/direct: real FILES publication/hash/quota/floor/cold helper; IO and stale paths zero receipt PASS; metadata RPCs are adapter stubs, no live distributed repair claim");return 0;
}
