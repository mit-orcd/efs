/* Staged append reservations and offline inspection of owned, stopped stores. */
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/meta_apply.h"
#include "efs/session.h"
#include "efs/lock.h"
#include "efs/kv_lsm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned markers,reservations,receipts,global_markers,global_reservations;
static int count(void *ctx,const uint8_t *key,uint32_t len,const uint8_t *value,uint32_t vl)
{
 (void)value;(void)vl;
 efs_ino_t ino=*(efs_ino_t *)ctx,target=0;
 if(len>=11)for(unsigned i=3;i<11;i++)target=target<<8|key[i];
 receipts+=len==31 && key[2]==EFS_KV_KIND_APPEND_RECEIPT && target==ino;
 if(len>=3){
  global_markers+=key[2]==EFS_KV_KIND_CLOSE_STEP;
  global_reservations+=key[2]==EFS_KV_KIND_APPEND_RSV;
  if(len>=11 && target==ino){
   markers+=key[2]==EFS_KV_KIND_CLOSE_STEP;
   reservations+=key[2]==EFS_KV_KIND_APPEND_RSV;
  }
 }
 return 0;
}
int main(int argc,char **argv)
{
 if(argc==4 && !strcmp(argv[1],"inspect")) {
  struct efs_kv *kv=efs_kv_lsm_open(argv[2],NULL);assert(kv);
  efs_ino_t ino=strtoull(argv[3],NULL,10);unsigned open=0;uint64_t wm=0,frontier=0;
  assert(efs_kv_scan_from(kv,NULL,0,NULL,0,count,&ino)==EFS_OK);
  int rc=efs_meta_apply_append_state(kv,ino,&wm,&frontier,&open);
  printf("markers=%u reservations=%u append_rc=%d open=%u watermark=%llu frontier=%llu locks=%d receipts=%u global_markers=%u global_reservations=%u\n",
   markers,reservations,rc,open,(unsigned long long)wm,(unsigned long long)frontier,efs_lock_count(kv,ino,1),receipts,global_markers,global_reservations);
  efs_kv_lsm_close(kv);return 0;
 }
 int late=argc==6 && !strcmp(argv[1],"late");
 assert(late || (argc==7 && !strcmp(argv[1],"reserve")));
 unsigned port=atoi(argv[2]),total=late?1:atoi(argv[5]);efs_ino_t ino=strtoull(argv[3],NULL,10);
 uint8_t uuid[16];assert(strlen(argv[4])==32);
 for(unsigned i=0;i<16;i++){unsigned x;assert(sscanf(argv[4]+2*i,"%2x",&x)==1);uuid[i]=x;}
 uint64_t length=strtoull(argv[late?5:6],NULL,10),first=0;
 for(unsigned step=0;step<total;step++) {
  unsigned i=late?65:step+1;
  struct efs_msg_inode_append request={.export_id=1,.ino=ino,.len=length};
  uint8_t packet[sizeof(request)+EFS_APPEND_ACK_LEN];uint32_t epoch=1;uint64_t seq=i,ack=i-1;
  memcpy(packet,&request,sizeof(request));memcpy(packet+sizeof(request),uuid,16);
  memcpy(packet+sizeof(request)+16,&epoch,4);memcpy(packet+sizeof(request)+EFS_SESS_WIRE_LEN,&seq,8);
  memcpy(packet+sizeof(request)+EFS_APPEND_OPID_LEN,&ack,8);
  unsigned attempt;
  for(attempt=0;attempt<400;attempt++) {
   int fd=efs_connect_declared("127.0.0.1",port+attempt%4);if(fd<0){usleep(10000);continue;}
   efs_set_recv_timeout(fd,10000);efs_set_send_timeout(fd,10000);
   uint8_t type=0;void *body=NULL;uint32_t size=0;
   int rc=efs_send_msg(fd,EFS_MSG_INODE_APPEND,packet,sizeof(packet));
   if(!rc)rc=efs_recv_msg(fd,&type,&body,&size);
   close(fd);
   if(rc){free(body);usleep(10000);continue;}
   assert(type==EFS_MSG_INODE_APPEND_REPLY && size==sizeof(struct efs_msg_inode_reply));
   struct efs_msg_inode_reply reply;memcpy(&reply,body,sizeof(reply));free(body);
   if(reply.status==EFS_INODE_RPC_BUSY || reply.status==EFS_INODE_RPC_NOT_PRIMARY){usleep(10000);continue;}
   if(late) {assert(reply.status==EFS_INODE_RPC_STALE);puts("fresh sequence65 after durable revocation rejected STALE PASS");return 0;}
   assert(reply.status==EFS_INODE_RPC_OK);if(i==1)first=reply.inode.atime;
   assert(reply.inode.atime==first+(i-1)*length);break;
  }
  assert(attempt<400);
 }
 printf("reserved=%u first=%llu end=%llu\n",total,(unsigned long long)first,(unsigned long long)(first+total*length));
 return 0;
}
