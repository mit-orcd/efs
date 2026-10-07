/* Live D25 I23 gate: no fragment PUTs; every intent has an invalid FileID,
 * so only session/terminal-receipt metadata can change. Run on test clusters.
 * cc -Iinclude tests/live/publication_session.c libefs.a -lpthread -lm -ldl -libverbs -o /tmp/publication-session-live
 * /tmp/publication-session-live host port disposable-ino /path/to/efs-mgmt
 */
#include "efs/publication.h"
#include "efs/network.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static const char *host,*mgmt;
static uint16_t port;
static char address[128],uuid_hex[33],shard_arg[16];
static void session_expect(const char *op,const char *arg,const char *epoch,int success)
{
    pid_t pid=fork();assert(pid>=0);
    if(!pid){
        if(epoch)execl(mgmt,mgmt,"raft-session",address,op,uuid_hex,arg,epoch,(char *)NULL);
        else if(arg)execl(mgmt,mgmt,"raft-session",address,op,uuid_hex,arg,(char *)NULL);
        else execl(mgmt,mgmt,"raft-session",address,op,uuid_hex,(char *)NULL);
        _exit(127);
    }
    int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && ((WEXITSTATUS(status)==0)==success));
}
static void session(const char *op,const char *arg,const char *epoch)
{ session_expect(op,arg,epoch,1); }
static void begin_fence(const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    uint8_t payload[19]={0};
    payload[0]=efs_raft_shard_group(efs_kv_session_shard(uuid));
    payload[1]=EFS_MD_CMD_SESSION;payload[2]=EFS_MD_SESS_BEGIN;memcpy(payload+3,uuid,16);
    char target[64];snprintf(target,sizeof(target),"%s",host);uint16_t target_port=port;
    for(unsigned attempt=0;attempt<8;attempt++) {
        int fd=efs_connect_tcp(target,target_port);assert(fd>=0);
        assert(!efs_set_recv_timeout(fd,30000));assert(!efs_set_send_timeout(fd,30000));
        assert(!efs_send_msg(fd,EFS_MSG_RAFT_MKFS,payload,sizeof(payload)));
        uint8_t type;void *reply;uint32_t n;
        assert(!efs_recv_msg(fd,&type,&reply,&n));close(fd);
        assert(type==EFS_MSG_RAFT_MKFS_REPLY && n==sizeof(struct efs_msg_raft_mkfs_reply));
        struct efs_msg_raft_mkfs_reply result;memcpy(&result,reply,n);free(reply);
        if(result.rc==EFS_OK)return;
        assert(result.rc==EFS_ERR_NOT_PRIMARY && result.leader_hint>=0);
        fd=efs_connect_tcp(host,port);assert(fd>=0);
        assert(!efs_set_recv_timeout(fd,30000));assert(!efs_set_send_timeout(fd,30000));
        assert(!efs_send_msg(fd,EFS_MSG_LIST_NODES,NULL,0));
        assert(!efs_recv_msg(fd,&type,&reply,&n));close(fd);
        assert(type==EFS_MSG_LIST_NODES_REPLY && n==sizeof(struct efs_msg_list_nodes_reply));
        struct efs_msg_list_nodes_reply *list=reply;assert(list->node_count<=EFS_MAX_NODES);int found=0;
        for(unsigned i=0;i<list->node_count;i++)if(list->nodes[i].id==(unsigned)result.leader_hint+1){
            snprintf(target,sizeof(target),"%s",list->nodes[i].addr);target_port=list->nodes[i].port;found=1;break;
        }
        free(reply);assert(found);
    }
    assert(!"BEGIN redirect limit");
}
static struct efs_msg_publication_reply request(const struct efs_msg_publication *req,int mode)
{
    int fd=efs_connect_tcp(host,port);assert(fd>=0);
    assert(!efs_set_recv_timeout(fd,30000));assert(!efs_set_send_timeout(fd,30000));
    uint8_t type=mode==2?EFS_MSG_PUBLICATION_RETIRE:mode?EFS_MSG_PUBLICATION_STATUS:EFS_MSG_PUBLICATION;
    uint8_t reply_type=mode==2?EFS_MSG_PUBLICATION_RETIRE_REPLY:mode?EFS_MSG_PUBLICATION_STATUS_REPLY:EFS_MSG_PUBLICATION_REPLY;
    assert(!efs_send_msg(fd,type,req,sizeof(*req)));
    uint8_t got;void *payload=NULL;uint32_t n;
    assert(!efs_recv_msg(fd,&got,&payload,&n));close(fd);
    assert(got==reply_type && n==sizeof(struct efs_msg_publication_reply));
    struct efs_msg_publication_reply out;memcpy(&out,payload,n);free(payload);return out;
}
int main(int argc,char **argv)
{
    if(argc!=5 && argc!=6){fprintf(stderr,"usage: %s host port disposable-ino efs-mgmt-path [same|cross]\n",argv[0]);return 2;}
    host=argv[1];port=(uint16_t)strtoul(argv[2],NULL,10);mgmt=argv[4];
    snprintf(address,sizeof(address),"%s:%u",host,port);
    struct efs_msg_publication req={0};req.rec.ino=strtoull(argv[3],NULL,10);assert(req.rec.ino>1);
    req.rec.file_generation=UINT64_MAX; /* deliberately impossible for fresh test fixture */
    req.rec.chunk_generation=1;req.rec.publish_flags=EFS_CHUNK_REC_F_CAPTURED_FILEID|EFS_CHUNK_REC_F_CAPTURED_EPOCH;
    req.id.session_epoch=1;req.id.seq=1;
    FILE *random=fopen("/dev/urandom","rb");assert(random);assert(fread(req.id.client_uuid,1,16,random)==16);fclose(random);
    for(unsigned i=0;i<16;i++)snprintf(uuid_hex+2*i,3,"%02x",req.id.client_uuid[i]);
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++)req.rec.nodes[i]=i+1;
    uint32_t shard=0;int found=0;
    for(unsigned ci=0;ci<EFS_META_LANES;ci++){
        shard=efs_kv_lane_shard(req.rec.ino,ci);
        if(efs_raft_shard_group(shard)==efs_raft_shard_group(efs_kv_inode_shard(req.rec.ino))){req.rec.chunk_index=ci;found=1;break;}
    }
    assert(found);snprintf(shard_arg,sizeof(shard_arg),"%u",shard);
    if(argc==6) {
        assert(!strcmp(argv[5],"same") || !strcmp(argv[5],"cross"));
        int same=!strcmp(argv[5],"same"),matched=0;
        random=fopen("/dev/urandom","rb");assert(random);
        for(unsigned n=0;n<128;n++) {
            assert(fread(req.id.client_uuid,1,16,random)==16);
            if((efs_raft_shard_group(efs_kv_session_shard(req.id.client_uuid))==efs_raft_shard_group(shard))==same){matched=1;break;}
        }
        fclose(random);assert(matched);
        for(unsigned i=0;i<16;i++)snprintf(uuid_hex+2*i,3,"%02x",req.id.client_uuid[i]);
    }
    struct efs_msg_publication_reply out=request(&req,0);
    assert(out.rpc.status==EFS_INODE_RPC_BUSY && out.state==EFS_PUBLICATION_UNKNOWN);
    session_expect("establish",shard_arg,"1",0); /* cannot establish an unknown session */
    session("create","1",NULL);
    session_expect("establish",shard_arg,"1",0); /* ACTIVE but not REGISTERed */
    session("register",shard_arg,"1");
    out=request(&req,0);assert(out.rpc.status==EFS_INODE_RPC_BUSY && !out.state);
    session("establish",shard_arg,"1");
    out=request(&req,0);assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_REJECTED && out.verdict==EFS_ERR_STALE);
    out=request(&req,2);assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_RETIRED);
    req.id.seq=2;out=request(&req,0);assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_REJECTED);
    begin_fence(req.id.client_uuid);
    session_expect("establish",shard_arg,"2",0); /* global barrier is still FENCING */
    session("fence",NULL,NULL);
    for(unsigned seq=1;seq<=2;seq++)for(int mode=0;mode<3;mode++){
        req.id.seq=seq;out=request(&req,mode);
        assert(out.rpc.status==EFS_INODE_RPC_STALE && out.state==EFS_PUBLICATION_UNKNOWN);
    }
    req.id.session_epoch=2;req.id.seq=1;
    out=request(&req,0);assert(out.rpc.status==EFS_INODE_RPC_BUSY && !out.state);
    session("register",shard_arg,"2");session("establish",shard_arg,"2");
    out=request(&req,0);assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_REJECTED);
    out=request(&req,2);assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_RETIRED);
    printf("live publication session gate PASS uuid=%s ino=%llu shard=%u lane_group=%u session_group=%u\n",uuid_hex,(unsigned long long)req.rec.ino,shard,efs_raft_shard_group(shard),efs_raft_shard_group(efs_kv_session_shard(req.id.client_uuid)));
    return 0;
}
