/* Private stores only. Durable admission with a lost reply, then cold recovery.
 * No fragment PUTs: an impossible FileID gives an exact terminal rejection. */
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
#include <unistd.h>
static unsigned base;
static const char *mgmt;
static void be32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-8*i));}
static void be64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-8*i));}
static int connect_node(unsigned node)
{
    int fd=efs_connect_tcp("127.0.0.1",(uint16_t)(base+node));assert(fd>=0);
    assert(!efs_set_recv_timeout(fd,15000));assert(!efs_set_send_timeout(fd,15000));return fd;
}
static void session(const char *op,const char *uuid,const char *arg,const char *epoch)
{
    char address[64];snprintf(address,sizeof(address),"127.0.0.1:%u",base);
    pid_t child=fork();assert(child>=0);
    if(!child){if(epoch)execl(mgmt,mgmt,"raft-session",address,op,uuid,arg,epoch,(char *)NULL);
        else execl(mgmt,mgmt,"raft-session",address,op,uuid,arg,(char *)NULL);
        _exit(127);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
}
static struct efs_msg_publication_reply request(const struct efs_msg_publication *req,int mode)
{
    int fd=connect_node(1); /* complete host routes to the authoritative lane */
    uint8_t type=mode==2?EFS_MSG_PUBLICATION_RETIRE:mode?EFS_MSG_PUBLICATION_STATUS:EFS_MSG_PUBLICATION;
    assert(!efs_send_msg(fd,type,req,sizeof(*req)));
    void *data=NULL;uint8_t got;uint32_t len;
    assert(!efs_recv_msg(fd,&got,&data,&len));close(fd);
    assert(got==(mode==2?EFS_MSG_PUBLICATION_RETIRE_REPLY:mode?EFS_MSG_PUBLICATION_STATUS_REPLY:EFS_MSG_PUBLICATION_REPLY));
    assert(len==sizeof(struct efs_msg_publication_reply));
    struct efs_msg_publication_reply out;memcpy(&out,data,len);free(data);return out;
}
int main(int argc,char **argv)
{
    assert(argc==6);base=(unsigned)atoi(argv[1]);mgmt=argv[2];
    const char *phase=argv[3],*file=argv[4];
    struct efs_msg_publication req={0};
    if(!strcmp(phase,"prepare")) {
        req.rec.ino=strtoull(argv[5],NULL,10);assert(req.rec.ino>1);
        req.rec.file_generation=UINT64_MAX;req.rec.chunk_generation=1;
        req.rec.publish_flags=EFS_CHUNK_REC_F_CAPTURED_FILEID|EFS_CHUNK_REC_F_CAPTURED_EPOCH;
        req.id.session_epoch=1;req.id.seq=1;
        FILE *random=fopen("/dev/urandom","rb");assert(random);
        assert(fread(req.id.client_uuid,1,16,random)==16);fclose(random);
        char uuid[33],shard_arg[16];
        for(unsigned i=0;i<16;i++)snprintf(uuid+2*i,3,"%02x",req.id.client_uuid[i]);
        unsigned shard=efs_kv_lane_shard(req.rec.ino,0);
        snprintf(shard_arg,sizeof(shard_arg),"%u",shard);
        session("create",uuid,"1",NULL);session("register",uuid,shard_arg,"1");session("establish",uuid,shard_arg,"1");
        for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++)req.rec.nodes[i]=i+1;
        FILE *saved=fopen(file,"wb");assert(saved);assert(fwrite(&req,1,sizeof(req),saved)==sizeof(req));assert(!fclose(saved));
        /* Raw replicated admission isolates the crash between the endpoint's
         * admission and publication proposals. Drop its original ACK. */
        uint8_t body[231]={0};body[0]=efs_raft_shard_group(shard);
        uint8_t *cmd=body+1;cmd[0]=EFS_MD_CMD_PUBLICATION_ADMIT;
        be64(cmd+1,req.rec.ino);be32(cmd+9,req.rec.chunk_index);
        uint8_t *q=cmd+29;
        for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++){be32(q,req.rec.nodes[i]);q+=4+EFS_HASH_SIZE;}
        q+=20;be64(q,1);be32(q+24,EFS_META_PROFILE_K2F1);q+=28;be64(q,UINT64_MAX);
        q+=17;assert(q==cmd+202);memcpy(q,req.id.client_uuid,16);be32(q+16,1);be64(q+20,1);
        /* Submit to each member: only the current leader accepts raw submit;
         * duplicates are bound to the same immutable intent. */
        for(unsigned node=0;node<4;node++) {
            int fd=connect_node(node);assert(!efs_send_msg(fd,EFS_MSG_RAFT_MKFS,body,sizeof(body)));close(fd);
        }
        struct efs_msg_publication_reply out={0};
        for(unsigned retry=0;retry<100;retry++) {
            out=request(&req,1);if(out.rpc.status==EFS_INODE_RPC_OK)break;usleep(100000);
        }
        assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_PENDING && out.verdict==EFS_META_PUBLICATION_PENDING);
        assert(out.receipt_index && out.receipt_term && out.receipt_group==efs_raft_shard_group(efs_kv_lane_shard(req.rec.ino,req.rec.chunk_index%EFS_META_LANES)));
        printf("pending intent locator group=%u index=%llu term=%llu\n",out.receipt_group,(unsigned long long)out.receipt_index,(unsigned long long)out.receipt_term);
        out=request(&req,2);assert(out.rpc.status==EFS_INODE_RPC_BUSY && !out.state);
        puts("pending intent survives lost admission ACK; early retirement refused PASS");
    } else {
        assert(!strcmp(phase,"finish"));FILE *saved=fopen(file,"rb");assert(saved);
        assert(fread(&req,1,sizeof(req),saved)==sizeof(req));fclose(saved);
        struct efs_msg_publication_reply out=request(&req,1);
        assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_PENDING);
        assert(out.receipt_index && out.receipt_term && out.receipt_group==efs_raft_shard_group(efs_kv_lane_shard(req.rec.ino,req.rec.chunk_index%EFS_META_LANES)));
        printf("pending intent locator group=%u index=%llu term=%llu\n",out.receipt_group,(unsigned long long)out.receipt_index,(unsigned long long)out.receipt_term);
        uint64_t pending_index=out.receipt_index;
        struct efs_msg_publication changed=req;changed.size++;
        out=request(&changed,0);assert(out.rpc.status!=EFS_INODE_RPC_OK && !out.state);
        out=request(&req,0);assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_REJECTED && out.verdict==EFS_ERR_STALE);
        assert(out.receipt_index>pending_index && out.receipt_term);
        printf("terminal rejection locator group=%u index=%llu term=%llu\n",out.receipt_group,(unsigned long long)out.receipt_index,(unsigned long long)out.receipt_term);
        out=request(&req,1);assert(out.state==EFS_PUBLICATION_REJECTED && out.verdict==EFS_ERR_STALE);
        out=request(&req,2);assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_RETIRED);
        out=request(&req,0);assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_RETIRED);
        puts("cold pending query, altered retry refusal, final rejection and replay floor PASS");
    }
    return 0;
}
