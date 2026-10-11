/* Authoritative inode attributes for atomic namespace refusal checks. */
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv)
{
    /* Predict fixture placement with the actual production hash. No RPC or
     * filesystem mutation is needed while rejecting candidate names. */
    if(argc==6 && !strcmp(argv[1],"choose")) {
        efs_ino_t parent=strtoull(argv[2],NULL,10);
        uint64_t salt=strtoull(argv[3],NULL,10);
        unsigned char used[EFS_KV_SHARD_MASK+1]={0};unsigned shard;
        int fresh=!strcmp(argv[5],"fresh");
        assert(fresh || !strcmp(argv[5],"member"));
        while(scanf("%u",&shard)==1){assert(shard<=EFS_KV_SHARD_MASK);used[shard]=1;}
        for(unsigned attempt=0;attempt<65536;attempt++) {
            char name[128];int count=snprintf(name,sizeof(name),"%s-%u",argv[4],attempt);
            assert(count>0 && count<(int)sizeof(name));
            shard=efs_kv_mkdir_shard(parent,name,salt);
            if((!used[shard])==fresh){printf("%u %s\n",shard,name);return 0;}
        }
        fprintf(stderr,"no namespace fixture placement within bounded search\n");
        return 1;
    }
    assert(argc==3);
    unsigned port=(unsigned)atoi(argv[1]);
    struct efs_msg_inode_getattr q={.export_id=1,.ino=strtoull(argv[2],NULL,10)};
    for(unsigned attempt=0;attempt<120;attempt++) {
        int fd=efs_connect_tcp("127.0.0.1",port+attempt%4);
        if(fd<0){usleep(10000);continue;}
        efs_set_recv_timeout(fd,1000);efs_set_send_timeout(fd,1000);
        int rc=efs_send_msg(fd,EFS_MSG_INODE_GETATTR,&q,sizeof(q));
        uint8_t type=0;void *body=NULL;uint32_t len=0;
        if(!rc)rc=efs_recv_msg(fd,&type,&body,&len);
        close(fd);
        if(rc){free(body);usleep(10000);continue;}
        assert(type==EFS_MSG_INODE_GETATTR_REPLY && len==sizeof(struct efs_msg_inode_reply));
        struct efs_msg_inode_reply reply;memcpy(&reply,body,sizeof(reply));free(body);
        if(reply.status==EFS_INODE_RPC_BUSY || reply.status==EFS_INODE_RPC_NOT_PRIMARY){usleep(10000);continue;}
        assert(reply.status==EFS_INODE_RPC_OK && reply.inode.ino==q.ino);
        const struct efs_inode *r=&reply.inode;
        printf("%llu %u %u %u %u %llu %u %llu %u\n",
            (unsigned long long)r->ino,r->uid,r->gid,r->mode,r->nlink,
            (unsigned long long)r->mtime,r->mtime_nsec,
            (unsigned long long)r->ctime,r->ctime_nsec);
        return 0;
    }
    return 1;
}
