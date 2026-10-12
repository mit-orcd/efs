#include "efs/protocol.h"
#include "efs/network.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static const char *hosts[]={"172.16.223.57","172.16.223.58","172.16.223.59","172.16.223.60"};
int main(int argc,char **argv){
 if(argc!=5)return 2;
 unsigned port=(unsigned)strtoul(argv[4],0,10);if(!port||port>65535)return 2;
 struct efs_msg_inode_getchunks req={.export_id=1,.ino=strtoull(argv[1],0,10),.max=1};
 uint64_t id=strtoull(argv[2],0,10),gen=strtoull(argv[3],0,10);
 for(unsigned round=0;round<3;round++)for(unsigned i=0;i<4;i++){
  int fd=efs_connect_tcp(hosts[i],port);if(fd<0)continue;
  efs_set_recv_timeout(fd,10000);efs_set_send_timeout(fd,10000);
  uint8_t type=0;void *raw=NULL;uint32_t n=0;
  int rc=efs_send_msg(fd,EFS_MSG_GETCHUNKS_PACKED,&req,sizeof(req))||efs_recv_msg(fd,&type,&raw,&n);close(fd);
  if(!rc&&type==EFS_MSG_GETCHUNKS_PACKED_REPLY&&n==sizeof(struct efs_msg_inode_getchunks_reply)){
   struct efs_msg_inode_getchunks_reply *r=raw;
   if(!r->status&&!efs_getchunks_reply_valid(&req,r)){
    int match=r->count==1&&r->recs[0].chunk_index==0&&(r->recs[0].publish_flags&EFS_CHUNK_REC_F_PACKED_BASE)&&r->recs[0].packed.segment_id==id&&r->recs[0].packed.segment_generation==gen;
    printf("{\"ino\":%llu,\"generation\":%llu,\"packed\":%d,\"segment_id\":%llu,\"segment_generation\":%llu}\n",(unsigned long long)req.ino,(unsigned long long)r->generation,match,(unsigned long long)r->recs[0].packed.segment_id,(unsigned long long)r->recs[0].packed.segment_generation);
    free(raw);return match?0:1;
   }
  }
  free(raw);
 }
 return 3;
}
