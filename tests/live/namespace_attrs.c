/* Authoritative inode attributes for atomic namespace refusal checks. */
#include "efs/protocol.h"
#include "efs/network.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char **argv)
{
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
