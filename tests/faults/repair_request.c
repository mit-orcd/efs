/* Private fixture driver, not an installed administration tool. */
#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include "efs/repair_job.h"
#include "efs/put_upload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc,char **argv)
{
    if(argc!=8){fprintf(stderr,"usage: repair_request HOST PORT INO OBJECT CI FI FRAGMENT_LEN\n");return 2;}
    int fd=efs_connect_tcp(argv[1],(uint16_t)strtoul(argv[2],NULL,10));if(fd<0)return 1;
    struct efs_conn *c=efs_conn_wrap_tcp(fd,0);if(!c)return 1;
    if(efs_rdma_transport()==EFS_TRANSPORT_RDMA && efs_rdma_client_upgrade(c)){efs_conn_destroy(c);return 1;}
    struct efs_msg_inode_writer_view q={.ino=strtoull(argv[3],NULL,10),.chunk_index=strtoul(argv[5],NULL,10)};
    void *reply=NULL;uint32_t len=0;uint8_t type=0;
    if(efs_conn_send_msg(c,EFS_MSG_INODE_WRITER_VIEW,&q,sizeof(q)) ||
       efs_conn_recv_msg(c,&type,&reply,&len) || type!=EFS_MSG_INODE_WRITER_VIEW_REPLY ||
       len!=sizeof(struct efs_msg_inode_writer_view_reply)){free(reply);efs_conn_destroy(c);return 1;}
    struct efs_msg_inode_writer_view_reply view;memcpy(&view,reply,sizeof(view));free(reply);reply=NULL;
    if(view.status!=EFS_INODE_RPC_OK || efs_writer_view_reply_valid(&q,&view)){efs_conn_destroy(c);return 1;}
    struct efs_repair_job j={.ino=q.ino,.inode_gen=view.generation,.object=strtoull(argv[4],NULL,10),
        .ci=q.chunk_index,.fi=strtoul(argv[6],NULL,10),.fragment_len=strtoul(argv[7],NULL,10),
        .epoch=1,.owner=1,.state=EFS_REPAIR_ADMITTED,.nodes={1,2,3}};
    FILE *random=fopen("/dev/urandom","rb");if(!random){efs_conn_destroy(c);return 1;}
    size_t n=fread(j.uuid,1,sizeof(j.uuid),random);fclose(random);if(n!=sizeof(j.uuid)){efs_conn_destroy(c);return 1;}
    j.target_store[0]=1;
    uint8_t request[4+EFS_BUILD_ID_LEN+EFS_REPAIR_JOB_BYTES]={0};
    efs_put_upload_wr32(request,EFS_VERSION_PACK);memcpy(request+4,EFS_BUILD_ID,strlen(EFS_BUILD_ID));
    if(efs_repair_job_encode(&j,request+4+EFS_BUILD_ID_LEN)){efs_conn_destroy(c);return 1;}
    int rc=1;
    if(!efs_conn_send_msg(c,EFS_MSG_REPAIR_REQUEST,request,sizeof(request)) &&
       !efs_conn_recv_msg(c,&type,&reply,&len) && type==EFS_MSG_REPAIR_REQUEST_REPLY && len==4){
        rc=(int32_t)efs_put_upload_rd32(reply);printf("repair ino=%llu object=%llu role=%u rc=%d transport=%s\n",
            (unsigned long long)j.ino,(unsigned long long)j.object,j.fi,rc,c->kind==EFS_CONN_RDMA?"rdma":"tcp");
    }
    free(reply);efs_conn_destroy(c);return rc?1:0;
}
