/* Private live RPC gate for sequenced lease effects. No production activation. */
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/meta_cmd.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void be(uint8_t *p,uint64_t v,unsigned n){while(n){p[--n]=(uint8_t)v;v>>=8;}}
int main(int argc,char **argv)
{
 if(argc==3){
  unsigned port=atoi(argv[1]);struct efs_msg_inode_getattr q={0};q.export_id=1;q.ino=strtoull(argv[2],0,10);
  for(unsigned a=0;a<120;a++){
   int fd=efs_connect_tcp("127.0.0.1",port+a%4);if(fd<0)continue;
   efs_set_recv_timeout(fd,1000);efs_set_send_timeout(fd,1000);
   int rc=efs_send_msg(fd,EFS_MSG_INODE_GETATTR,&q,sizeof(q));uint8_t type=0;void *body=NULL;uint32_t len=0;
   if(!rc)rc=efs_recv_msg(fd,&type,&body,&len);
   close(fd);
   if(rc){free(body);continue;}
   assert(type==EFS_MSG_INODE_GETATTR_REPLY && len==sizeof(struct efs_msg_inode_reply));
   struct efs_msg_inode_reply reply;memcpy(&reply,body,sizeof(reply));free(body);
   if(reply.status==EFS_INODE_RPC_BUSY || reply.status==EFS_INODE_RPC_NOT_PRIMARY){usleep(10000);continue;}
   printf("status=%u gen=%llu nlink=%u shard=%u\n",reply.status,(unsigned long long)1 /* fresh CREATE generation; fixture never truncates */,reply.inode.nlink,efs_kv_inode_shard(q.ino));return 0;
  }
  return 1;
 }
 assert(argc==11);unsigned port=atoi(argv[1]);efs_ino_t ino=strtoull(argv[2],0,10);
 uint64_t gen=strtoull(argv[3],0,10),seq=strtoull(argv[5],0,10),ack=strtoull(argv[6],0,10);
 unsigned flags=atoi(argv[7]),probe=atoi(argv[8]);int expected=atoi(argv[9]),discard=atoi(argv[10]);
 uint8_t p[1+EFS_MD_SESS_LEASE_EDGE_LEN]={0};unsigned shard=efs_kv_inode_shard(ino);
 p[0]=efs_raft_shard_group(shard);p[1]=EFS_MD_CMD_SESSION;p[2]=probe?EFS_MD_SESS_LEASE_PROBE:EFS_MD_SESS_LEASE_EDGE;
 assert(strlen(argv[4])==32);for(unsigned i=0;i<16;i++){unsigned b;assert(sscanf(argv[4]+2*i,"%2x",&b)==1);p[3+i]=b;}
 be(p+19,1,4);be(p+23,seq,8);be(p+31,ack,8);be(p+39,ino,8);be(p+47,gen,8);be(p+55,flags,4);
 for(unsigned attempt=0;attempt<120;attempt++){
  int fd=efs_connect_tcp("127.0.0.1",port+attempt%4);if(fd<0){usleep(10000);continue;}
  efs_set_recv_timeout(fd,1000);efs_set_send_timeout(fd,1000);
  int rc=efs_send_msg(fd,EFS_MSG_RAFT_MKFS,p,sizeof(p));
  /* Deliberately discard a reply, then resolve solely through a read-only probe. */
  if(discard && !rc){usleep(200000);close(fd);puts("reply discarded");return 0;}
  uint8_t type=0;void *body=NULL;uint32_t len=0;
  if(!rc)rc=efs_recv_msg(fd,&type,&body,&len);
  close(fd);
  if(rc){free(body);usleep(10000);continue;}
  assert(type==EFS_MSG_RAFT_MKFS_REPLY && len==sizeof(struct efs_msg_raft_mkfs_reply));
  struct efs_msg_raft_mkfs_reply reply;memcpy(&reply,body,sizeof(reply));free(body);
  if(reply.rc==EFS_ERR_NOT_PRIMARY || (reply.rc==EFS_ERR_BUSY && expected!=EFS_ERR_BUSY)){usleep(10000);continue;}
  printf("rc=%d known=%llu shard=%u group=%u\n",reply.rc,(unsigned long long)reply.salt,shard,p[0]);
  if(reply.rc!=expected){fprintf(stderr,"expected rc=%d\n",expected);return 1;}
  if(probe && expected==EFS_OK)assert(reply.salt==1);
  return 0;
 }
 fprintf(stderr,"lease edge RPC deadline expired\n");return 1;
}
