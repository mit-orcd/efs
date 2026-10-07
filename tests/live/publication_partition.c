/* An owned partition fixture calls this through real typed-publication RPCs.
 * An impossible FileID permits a durable rejected receipt without data PUTs. */
#include "efs/publication.h"
#include "efs/network.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv)
{
    assert(argc==5);
    struct efs_msg_publication req={0};
    req.rec.ino=strtoull(argv[3],NULL,10);assert(req.rec.ino>1);
    req.rec.file_generation=UINT64_MAX;
    req.rec.chunk_generation=1;
    req.rec.publish_flags=EFS_CHUNK_REC_F_CAPTURED_FILEID|EFS_CHUNK_REC_F_CAPTURED_EPOCH;
    req.id.client_uuid[15]=2;req.id.session_epoch=1;req.id.seq=1;
    for(unsigned i=0;i<EFS_META_LANES;i++)
        if(efs_raft_shard_group(efs_kv_lane_shard(req.rec.ino,i))==0){req.rec.chunk_index=i;break;}
    unsigned shard=efs_kv_lane_shard(req.rec.ino,req.rec.chunk_index);
    assert(efs_raft_shard_group(shard)==0);
    if(!strcmp(argv[4],"describe")){printf("shard=%u\n",shard);return 0;}
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++)req.rec.nodes[i]=i+1;
    int fd=efs_connect_tcp(argv[1],(uint16_t)atoi(argv[2]));assert(fd>=0);
    assert(!efs_set_recv_timeout(fd,15000));assert(!efs_set_send_timeout(fd,15000));
    int query=!strcmp(argv[4],"query");assert(query || !strcmp(argv[4],"prepare"));
    assert(!efs_send_msg(fd,query?EFS_MSG_PUBLICATION_STATUS:EFS_MSG_PUBLICATION,&req,sizeof(req)));
    uint8_t type;void *body=NULL;uint32_t length;
    assert(!efs_recv_msg(fd,&type,&body,&length));close(fd);
    assert(type==(query?EFS_MSG_PUBLICATION_STATUS_REPLY:EFS_MSG_PUBLICATION_REPLY));
    assert(length==sizeof(struct efs_msg_publication_reply));
    struct efs_msg_publication_reply out;memcpy(&out,body,sizeof(out));free(body);
    printf("status=%u state=%u verdict=%d\n",out.rpc.status,out.state,out.verdict);
    return 0;
}
