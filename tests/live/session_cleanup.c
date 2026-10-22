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
        int fd=efs_connect_declared("127.0.0.1",port+attempt%4);
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
    uint8_t p[27]={0};p[0]=efs_raft_shard_group(shard);p[1]=EFS_MD_CMD_SESSION;
    p[2]=EFS_MD_SESS_LEASE_LOCK_NEXT;memcpy(p+3,uuid,16);be(p+19,1,4);be(p+23,shard,4);
    return rpc(p,sizeof(p),expected);
}
int main(int argc,char **argv)
{
    assert(argc==17);port=(unsigned)strtoul(argv[2],NULL,10);assert(strlen(argv[3])==32);
    for(unsigned i=0;i<16;i++){unsigned b;assert(sscanf(argv[3]+2*i,"%2x",&b)==1);uuid[i]=(uint8_t)b;}
    uint64_t inos[13];for(unsigned i=0;i<13;i++)inos[i]=strtoull(argv[4+i],NULL,10);
    shard=efs_kv_inode_shard(inos[0]);
    for(unsigned i=0;i<13;i++){assert(efs_kv_inode_shard(inos[i])==shard);if(i)assert(inos[i]>inos[i-1]);}
    unsigned group=efs_raft_shard_group(shard),owner=efs_raft_shard_group(efs_kv_session_shard(uuid));
    if(!strcmp(argv[1],"prepare")) {
        session(EFS_MD_SESS_CREATE,owner,1,0,22,EFS_OK);
        session(EFS_MD_SESS_REGISTER,owner,1,shard,26,EFS_OK);
        session(EFS_MD_SESS_ESTABLISH,group,1,shard,26,EFS_OK);
        for(unsigned i=0;i<13;i++) {
            uint8_t p[39]={0};p[0]=group;p[1]=EFS_MD_CMD_SESSION;p[2]=EFS_MD_SESS_LEASE_OPEN;
            memcpy(p+3,uuid,16);be(p+19,1,4);be(p+23,inos[i],8);be(p+31,1,8);rpc(p,sizeof(p),EFS_OK);
        }
        for(unsigned i=0;i<8;i++) {
            uint8_t p[66]={0};p[0]=group;p[1]=EFS_MD_CMD_LOCK;p[2]=EFS_MD_LOCK_GRANT;
            be(p+3,inos[0],8);be(p+11,1,8);p[19]=EFS_LOCK_FCNTL;p[20]=EFS_LOCK_EX;
            be(p+21,i*2,8);be(p+29,i*2+1,8);p[37]=EFS_LOCK_PROC;be(p+38,i+1,8);
            memcpy(p+46,uuid,16);be(p+62,1,4);rpc(p,sizeof(p),EFS_OK);
        }
        next(EFS_ERR_BUSY);puts("active cleanup refused PASS");
    } else if(!strcmp(argv[1],"first")) {
        session(EFS_MD_SESS_BEGIN,owner,0,0,18,EFS_OK);
        session(EFS_MD_SESS_FENCE_LOC,group,2,shard,26,EFS_OK);
        next(EFS_ERR_BUSY); /* local rejection alone cannot authorize ownership cleanup */
        session(EFS_MD_SESS_ACK,owner,shard,0,22,EFS_OK);
        /* Lease admission also registers guarded metadata participant shards.
         * Do not assume the hand-registered inode shard is the frozen set. */
        session(EFS_MD_SESS_FINISH,owner,2,0,22,EFS_ERR_BUSY);
        for(unsigned word=0;word<EFS_SESSION_BITMAP/8u;word++) {
            uint8_t query[23]={0};query[0]=owner;query[1]=EFS_MD_CMD_SESSION;
            memcpy(query+3,uuid,16);be(query+19,EFS_SESSION_BITS+word,4);
            uint64_t bits=rpc(query,sizeof(query),EFS_OK).salt;
            for(unsigned bit=0;bit<64;bit++)if(bits&(1ull<<bit)) {
                unsigned touched=word*64+bit;
                printf("cleanup frozen touched shard=%u\n",touched);
                session(EFS_MD_SESS_FENCE_LOC,efs_raft_shard_group(touched),2,touched,26,EFS_OK);
                session(EFS_MD_SESS_ACK,owner,touched,0,22,EFS_OK);
            }
        }
        session(EFS_MD_SESS_STORAGE_FENCE,owner,2,0,22,EFS_OK);
        session(EFS_MD_SESS_FINISH,owner,2,0,22,EFS_OK);
        session(EFS_MD_SESS_FINISH,owner,2,0,22,EFS_OK); /* exact retry */
        assert(next(EFS_OK).salt==0);
        /* Retry the exact first committed set; never select the next five. */
        uint8_t p[28+66*8]={0};p[0]=group;p[1]=EFS_MD_CMD_SESSION;
        p[2]=EFS_MD_SESS_LEASE_LOCK_BATCH;memcpy(p+3,uuid,16);be(p+19,1,4);be(p+23,shard,4);p[27]=8;
        for(unsigned i=0;i<8;i++) {
            uint8_t key[EFS_KV_KEY_MAX];uint32_t len;
            assert(!efs_kv_key_lease(shard,inos[i],1,uuid,1,key,&len) && len==39);
            p[28+66*i]=(uint8_t)len;memcpy(p+29+66*i,key,len);
        }
        rpc(p,sizeof(p),EFS_OK);puts("global barrier and exact first-batch replay PASS");
    } else {
        assert(!strcmp(argv[1],"finish"));
        unsigned rounds=0;
        while(!next(EFS_OK).salt)assert(++rounds<8);
        assert(next(EFS_OK).salt==1);puts("cold remaining lease/lock phase drained PASS");
    }
    return 0;
}
