/* CREATE initial lease: lost adoption, cold replay, unlink and last close. */
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/opid.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static unsigned port;
static uint64_t owner;
static struct efs_msg_inode_reply call(unsigned type,const void *request,unsigned size,unsigned seq)
{
    uint8_t packet[1024];assert(size+EFS_OPID_WIRE_LEN<=sizeof(packet));
    memcpy(packet,request,size);
    unsigned length=size;
    if(seq){struct efs_opid_req q={0};memcpy(q.id.client_uuid,&owner,sizeof(owner));q.id.client_uuid[15]=93;q.id.session_epoch=1;q.id.seq=seq;efs_opid_req_pack(&q,packet+size);length+=EFS_OPID_WIRE_LEN;}
    for(unsigned attempt=0;attempt<400;attempt++){
        int fd=efs_connect_tcp("127.0.0.1",port+attempt%4);
        if(fd<0){usleep(10000);continue;}
        efs_set_recv_timeout(fd,5000);efs_set_send_timeout(fd,5000);
        int rc=efs_send_msg(fd,type,packet,length);
        void *body=NULL;unsigned len=0;uint8_t reply_type=0;
        if(!rc)rc=efs_recv_msg(fd,&reply_type,&body,&len);
        close(fd);
        if(rc){free(body);usleep(10000);continue;}
        assert(reply_type==type+1 && len==sizeof(struct efs_msg_inode_reply));
        struct efs_msg_inode_reply reply;memcpy(&reply,body,sizeof(reply));free(body);
        if(reply.status==EFS_INODE_RPC_BUSY || reply.status==EFS_INODE_RPC_NOT_PRIMARY){usleep(10000);continue;}
        return reply;
    }
    abort();
}
int main(int argc,char **argv)
{
    assert(argc==5);port=atoi(argv[1]);efs_ino_t parent=strtoull(argv[2],NULL,10);
    owner=strtoull(argv[3],NULL,10);efs_ino_t expected=strtoull(argv[4],NULL,10);
    assert(parent && owner);
    struct efs_msg_inode_create create={0};create.export_id=1;create.parent=parent;
    create.mode=S_IFREG|0600;create.flags=EFS_CREATE_F_HOLD;create.owner=owner;
    unsigned shard=0;
    for(unsigned i=0;i<10000;i++){
        snprintf(create.name,sizeof(create.name),"lease-%u",i);
        shard=efs_kv_dentry_shard(parent,create.name,EFS_META_LAYOUT_HASHED);
        if(efs_raft_shard_group(shard)!=efs_raft_shard_group(efs_kv_inode_shard(parent)))break;
    }
    assert(efs_raft_shard_group(shard)!=efs_raft_shard_group(efs_kv_inode_shard(parent)));
    struct efs_msg_inode_reply reply=call(EFS_MSG_INODE_CREATE,&create,sizeof(create),1);
    assert(reply.status==EFS_INODE_RPC_OK && reply.inode.ino);
    efs_ino_t ino=reply.inode.ino;
    if(!expected){printf("%llu %s %u\n",(unsigned long long)ino,create.name,shard);return 0;}
    assert(ino==expected);
    struct efs_msg_inode_unlink unlink={0};unlink.export_id=1;unlink.parent=parent;
    strcpy(unlink.name,create.name);
    reply=call(EFS_MSG_INODE_UNLINK,&unlink,sizeof(unlink),2);assert(reply.status==EFS_INODE_RPC_OK);
    struct efs_msg_inode_getattr getattr={0};getattr.export_id=1;getattr.ino=ino;
    reply=call(EFS_MSG_INODE_GETATTR,&getattr,sizeof(getattr),0);
    assert(reply.status==EFS_INODE_RPC_OK && reply.inode.ino==ino && reply.inode.nlink==0);
    struct efs_msg_inode_hold close={0};close.export_id=1;close.ino=ino;close.owner=owner;
    reply=call(EFS_MSG_INODE_HOLD,&close,sizeof(close),0);assert(reply.status==EFS_INODE_RPC_OK);
    for(unsigned i=0;i<1000;i++){
        reply=call(EFS_MSG_INODE_GETATTR,&getattr,sizeof(getattr),0);
        if(reply.status==EFS_INODE_RPC_NOT_FOUND){puts("cold CREATE replay/open-unlinked/last-close PASS");return 0;}
        assert(reply.status==EFS_INODE_RPC_OK);usleep(10000);
    }
    abort();
}
