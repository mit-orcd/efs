/* Private stores only: real immutable fragments and W88 history eviction. */
#include "efs/erasure.h"
#include "efs/checksum.h"
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
static void rpc(unsigned node,uint8_t type,const void *req,uint32_t qlen,
                uint8_t expected,void *out,uint32_t olen)
{
    int fd=connect_node(node);assert(!efs_send_msg(fd,type,req,qlen));
    void *data=NULL;uint8_t got;uint32_t len;
    assert(!efs_recv_msg(fd,&got,&data,&len));close(fd);
    assert(got==expected && len==olen);memcpy(out,data,len);free(data);
}
static struct efs_chunk_rec observed(const struct efs_msg_publication *req)
{
    struct efs_msg_inode_getchunks q={.export_id=1,.ino=req->rec.ino,.start=req->rec.chunk_index,.max=1,.generation=req->rec.file_generation};
    struct efs_msg_inode_getchunks_reply *out=calloc(1,sizeof(*out));assert(out);
    for(unsigned attempt=0;attempt<20;attempt++){
        rpc(1,EFS_MSG_INODE_GETCHUNKS,&q,sizeof(q),EFS_MSG_INODE_GETCHUNKS_REPLY,out,sizeof(*out));
        if(out->status==EFS_INODE_RPC_OK)break;
        usleep(100000);
    }
    assert(out->status==EFS_INODE_RPC_OK && out->count==1);
    struct efs_chunk_rec result=out->recs[0];free(out);return result;
}
static void upload(struct efs_msg_publication *req,const uint8_t *chunk)
{
    uint32_t size=EFS_MIN_CHUNK_SIZE,fraglen=size/2;
    uint8_t *fragments[3];for(unsigned i=0;i<3;i++){fragments[i]=malloc(fraglen);assert(fragments[i]);}
    assert(efs_encode_chunk(chunk,size,size,fragments)==EFS_OK);
    for(unsigned i=0;i<3;i++){
        struct efs_msg_put_chunk q={.export_id=1,.ino=req->rec.ino,.chunk_index=req->rec.chunk_index,
            .fragment_index=i,.data_len=fraglen,.chunk_generation=req->rec.chunk_generation,.path_hint=0};
        efs_hash(fragments[i],fraglen,q.checksum);memcpy(req->rec.checksums[i],q.checksum,EFS_HASH_SIZE);req->rec.nodes[i]=i+1;
        uint8_t *body=malloc(sizeof(q)+fraglen);assert(body);memcpy(body,&q,sizeof(q));memcpy(body+sizeof(q),fragments[i],fraglen);
        int fd=connect_node(i);assert(!efs_send_msg(fd,EFS_MSG_PUT_CHUNK,body,sizeof(q)+fraglen));free(body);
        uint8_t type;uint32_t len;void *reply=NULL;assert(!efs_recv_msg(fd,&type,&reply,&len));close(fd);
        assert(type==EFS_MSG_PUT_CHUNK_REPLY && len>=1 && *(uint8_t *)reply==EFS_PUT_CHUNK_OK);free(reply);free(fragments[i]);
    }
}
static void committed(const struct efs_msg_publication *req,int lost_ack)
{
    if(lost_ack){int fd=connect_node(1);assert(!efs_send_msg(fd,EFS_MSG_PUBLICATION,req,sizeof(*req)));close(fd);}
    struct efs_msg_publication_reply out={0};
    for(unsigned retry=0;retry<40;retry++){
        out=request(req,lost_ack?1:0);
        if(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_COMMITTED)break;
        if(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_PENDING)lost_ack=0;
        usleep(100000);
    }
    assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_COMMITTED && out.verdict==EFS_OK);
}
int main(int argc,char **argv)
{
    assert(argc==6);base=(unsigned)atoi(argv[1]);mgmt=argv[2];const char *phase=argv[3],*file=argv[4];
    struct efs_msg_publication first={0};
    if(!strcmp(phase,"prepare")){
        first.rec.ino=strtoull(argv[5],NULL,10);assert(first.rec.ino>1);
        int cross=getenv("EFS_TEST_CROSS_GROUP")!=NULL,found=0;
        for(unsigned ci=0;ci<EFS_META_LANES;ci++){
            int different=efs_raft_shard_group(efs_kv_lane_shard(first.rec.ino,ci))!=efs_raft_shard_group(efs_kv_inode_shard(first.rec.ino));
            if(different==cross){first.rec.chunk_index=ci;found=1;break;}
        }
        assert(found);
        struct efs_msg_inode_writer_view q={.ino=first.rec.ino,.chunk_index=first.rec.chunk_index};
        struct efs_msg_inode_writer_view_reply view;
        for(unsigned retry=0;retry<30;retry++){
            rpc(1,EFS_MSG_INODE_WRITER_VIEW,&q,sizeof(q),EFS_MSG_INODE_WRITER_VIEW_REPLY,&view,sizeof(view));
            if(view.status==EFS_INODE_RPC_OK)break;
            usleep(100000);
        }
        assert(view.status==EFS_INODE_RPC_OK && view.generation);
        if(cross){
            /* Existing servers learn export geometry from fragment PUTs. Use
             * the exact first immutable object, not a disposable conflicting
             * offset or assumed authority row, before cold lane bootstrap. */
            uint8_t first_body[EFS_MIN_CHUNK_SIZE]={0};first_body[0]='a';
            first.rec.chunk_generation=100;upload(&first,first_body);
            struct efs_msg_lane_bootstrap bootstrap={.export_id=1,.ino=first.rec.ino,
                .generation=view.generation,.chunk_index=first.rec.chunk_index,.chunk_size=EFS_MIN_CHUNK_SIZE};
            struct efs_msg_lane_writer_view_reply lane;
            for(unsigned retry=0;retry<30;retry++){
                rpc(1,EFS_MSG_LANE_BOOTSTRAP,&bootstrap,sizeof(bootstrap),EFS_MSG_LANE_BOOTSTRAP_REPLY,&lane,sizeof(lane));
                if(lane.view.status==EFS_INODE_RPC_OK)break;
                usleep(100000);
            }
            struct efs_msg_lane_writer_view exact={first.rec.ino,view.generation,first.rec.chunk_index,EFS_MIN_CHUNK_SIZE};
            printf("lane bootstrap status=%u generation=%llu epoch=%llu chunk_size=%u validation=%d\n",
                lane.view.status,(unsigned long long)lane.view.generation,
                (unsigned long long)lane.view.authority_epoch,lane.chunk_size,
                efs_lane_writer_view_reply_valid(&exact,&lane));fflush(stdout);
            assert(lane.view.status==EFS_INODE_RPC_OK && !efs_lane_writer_view_reply_valid(&exact,&lane));
            view=lane.view;
        }
        printf("publication lane: cross_group=%d chunk_index=%u\n",cross,first.rec.chunk_index);fflush(stdout);
        first.rec.file_generation=view.generation;first.rec.publish_epoch=view.authority_epoch;
        first.rec.publish_flags=EFS_CHUNK_REC_F_CAPTURED_FILEID|EFS_CHUNK_REC_F_CAPTURED_EPOCH;
        first.id.session_epoch=1;first.id.seq=100;first.rec.chunk_generation=100;first.rec.delta_len=1;
        FILE *random=fopen("/dev/urandom","rb");assert(random);assert(fread(first.id.client_uuid,1,16,random)==16);fclose(random);
        char uuid[33],shard[16];for(unsigned i=0;i<16;i++)snprintf(uuid+2*i,3,"%02x",first.id.client_uuid[i]);
        snprintf(shard,sizeof(shard),"%u",efs_kv_lane_shard(first.rec.ino,first.rec.chunk_index));
        session("create",uuid,"1",NULL);session("register",uuid,shard,"1");session("establish",uuid,shard,"1");
        uint32_t size=EFS_MIN_CHUNK_SIZE;first.size=((uint64_t)first.rec.chunk_index+1)*size;
        uint8_t *body=calloc(1,size);assert(body);struct efs_msg_publication req=first;
        for(unsigned i=0;i<EFS_CHUNK_DELTA_MAX;i++){
            req=first;req.id.seq=100+i;req.rec.chunk_generation=100+i;req.rec.delta_off=i;
            body[i]=(uint8_t)('a'+i);upload(&req,body);committed(&req,i==0);if(i==0)first=req;
        }
        struct efs_chunk_rec old=observed(&req);
        assert(old.delta_base_n==EFS_CHUNK_DELTA_MAX);
        req=first;req.id.seq=200;req.rec.chunk_generation=200;req.rec.delta_len=0;
        req.rec.delta_base_n=old.delta_base_n;req.rec.delta_base_seq=old.delta_base_seq;
        upload(&req,body);committed(&req,0);
        old=observed(&req);
        req.id.seq=201;req.rec.chunk_generation=201;req.rec.base_gen=200;
        req.rec.delta_base_n=old.delta_base_n;req.rec.delta_base_seq=old.delta_base_seq;
        memset(body,'Z',size);upload(&req,body);committed(&req,0);
        req=first;req.id.seq=202;req.rec.chunk_generation=202;req.rec.delta_off=1000;
        body[1000]='Q';upload(&req,body);committed(&req,0);
        old=observed(&req);for(unsigned i=0;i<old.delta_base_n;i++)assert(old.deltas[i].generation!=100);
        committed(&first,0); /* original lost final ACK after full history eviction */
        old=observed(&req);assert(old.chunk_generation==201);
        for(unsigned i=0;i<old.delta_base_n;i++)assert(!(old.deltas[i].generation==100 && old.deltas[i].len));
        FILE *saved=fopen(file,"wb");assert(saved);assert(fwrite(&first,1,sizeof(first),saved)==sizeof(first));assert(!fclose(saved));
        char expected[1024];snprintf(expected,sizeof(expected),"%s.expected",file);saved=fopen(expected,"wb");assert(saved);
        for(unsigned i=0;i<first.rec.chunk_index;i++){uint8_t zero[EFS_MIN_CHUNK_SIZE]={0};assert(fwrite(zero,1,size,saved)==size);}
        assert(fwrite(body,1,size,saved)==size);assert(!fclose(saved));free(body);
        puts("real fragments: eight spans, fold, overwrite, trailer eviction and lost-final-ACK replay PASS");
    } else {
        assert(!strcmp(phase,"finish") || !strcmp(phase,"verify-retired"));
        FILE *saved=fopen(file,"rb");assert(saved);assert(fread(&first,1,sizeof(first),saved)==sizeof(first));fclose(saved);
        if(!strcmp(phase,"verify-retired")){
            struct efs_msg_publication_reply out=request(&first,1);
            assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_RETIRED);
            out=request(&first,0);
            assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_RETIRED);
            puts("cold replay floor: query and resubmit remain retired PASS");
            return 0;
        }
        struct efs_msg_publication altered=first;
        altered.rec.checksums[0][0]^=1;
        struct efs_msg_publication_reply bad=request(&altered,0);
        assert(bad.rpc.status==EFS_INODE_RPC_ERROR && bad.state==EFS_PUBLICATION_UNKNOWN);
        bad=request(&altered,1);assert(bad.rpc.status==EFS_INODE_RPC_ERROR && bad.state==EFS_PUBLICATION_UNKNOWN);
        committed(&first,0); /* exact result recovers across all-daemon restart */
        /* The driver proved GC removed the original object before restart. The final
         * receipt and subsequent floor must never authorize reintroduction. */
        uint8_t body[EFS_MIN_CHUNK_SIZE]={0};body[0]='a';struct efs_msg_publication same=first;
        upload(&same,body);assert(!memcmp(same.rec.checksums,first.rec.checksums,sizeof(first.rec.checksums)));
        committed(&first,0);
        struct efs_msg_publication_reply out=request(&first,2);assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_RETIRED);
        out=request(&first,0);assert(out.rpc.status==EFS_INODE_RPC_OK && out.state==EFS_PUBLICATION_RETIRED);
        struct efs_chunk_rec old=observed(&first);assert(old.chunk_generation==201);
        for(unsigned i=0;i<old.delta_base_n;i++)assert(!(old.deltas[i].generation==100 && old.deltas[i].len));
        puts("cold terminal recovery, old-object re-PUT and durable replay-floor refusal PASS");
    }
    return 0;
}
