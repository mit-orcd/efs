/* Private staged bounded lease/lock cleanup RPC gate. No automatic activation. */
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include "efs/session.h"
#include "efs/lock.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned port,shard;
static uint8_t uuid[16];
static void be(uint8_t *p,uint64_t v,unsigned n){while(n){p[--n]=(uint8_t)v;v>>=8;}}
static struct efs_msg_raft_mkfs_reply rpc(const uint8_t *p,unsigned n,int expected)
{
    for(unsigned attempt=0;attempt<120;attempt++) {
        int fd=efs_connect_tcp("127.0.0.1",port+attempt%4);
        if(fd<0){usleep(10000);continue;}
        assert(!efs_set_recv_timeout(fd,2000) && !efs_set_send_timeout(fd,2000));
        int rc=efs_send_msg(fd,EFS_MSG_RAFT_MKFS,p,n);uint8_t type=0;void *body=NULL;uint32_t len=0;
        if(!rc)rc=efs_recv_msg(fd,&type,&body,&len);
        close(fd);
        if(rc){free(body);usleep(10000);continue;}
        assert(type==EFS_MSG_RAFT_MKFS_REPLY && len==sizeof(struct efs_msg_raft_mkfs_reply));
        struct efs_msg_raft_mkfs_reply reply;memcpy(&reply,body,len);free(body);
        if(reply.rc==EFS_ERR_NOT_PRIMARY || (reply.rc==EFS_ERR_BUSY && expected!=EFS_ERR_BUSY)) {
            usleep(10000);continue;
        }
        printf("sub=%u rc=%d salt=%llu index=%llu\n",p[2],reply.rc,
               (unsigned long long)reply.salt,(unsigned long long)reply.index);fflush(stdout);
        assert(reply.rc==expected);return reply;
    }
    assert(!"session cleanup RPC deadline");abort();
}
static void session(unsigned sub,unsigned group,unsigned value,unsigned target,unsigned length,int expected)
{
    uint8_t p[27]={0};p[0]=group;p[1]=EFS_MD_CMD_SESSION;p[2]=sub;
    memcpy(p+3,uuid,16);be(p+19,value,4);be(p+23,target,4);rpc(p,length+1,expected);
}
static struct efs_msg_raft_mkfs_reply next(int expected)
{
 uint8_t p[26]={0};p[0]=efs_raft_shard_group(shard);p[1]=EFS_MD_REQ_SESSION_APPEND_NEXT;
 memcpy(p+2,uuid,16);be(p+18,1,4);be(p+22,shard,4);return rpc(p,sizeof(p),expected);
}
int main(int argc,char **argv)
{
 assert(argc==5);port=atoi(argv[2]);assert(strlen(argv[3])==32);
 for(unsigned i=0;i<16;i++){unsigned b;assert(sscanf(argv[3]+2*i,"%2x",&b)==1);uuid[i]=b;}
 shard=efs_kv_inode_shard(strtoull(argv[4],NULL,10));
 unsigned group=efs_raft_shard_group(shard),owner=efs_raft_shard_group(efs_kv_session_shard(uuid));
 assert(!strcmp(argv[1],"fence-half"));
 next(EFS_ERR_BUSY);
 session(EFS_MD_SESS_BEGIN,owner,0,0,18,EFS_OK);
 session(EFS_MD_SESS_FENCE_LOC,group,2,shard,26,EFS_OK);
 next(EFS_ERR_BUSY);
 session(EFS_MD_SESS_ACK,owner,shard,0,22,EFS_OK);
 session(EFS_MD_SESS_FINISH,owner,0,0,18,EFS_OK);
 for(unsigned i=0;i<32;i++)assert(next(EFS_OK).salt==0);
 puts("completed global barrier and one-reservation transactions: 32 of 64 PASS");
 return 0;
}
