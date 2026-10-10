/* Real admitted cache bytes, lost publication/retirement ACKs, concurrent
 * rewrite and peer preservation. Private fixture stores only. */
#include "../../src/client/client_internal.h"
#include "efs/publication_ack.h"
#include "efs/writer_state.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned base_port;
static int connect_node(unsigned node)
{
    int fd=efs_connect_tcp("127.0.0.1",base_port+node);assert(fd>=0);
    assert(!efs_set_recv_timeout(fd,15000));assert(!efs_set_send_timeout(fd,15000));return fd;
}
static void drop_reply(const struct efs_msg_publication *request,int retire)
{
    for(unsigned node=0;node<4;node++) {
        int fd=connect_node(node);
        assert(!efs_send_msg(fd,retire?EFS_MSG_PUBLICATION_RETIRE:EFS_MSG_PUBLICATION,request,sizeof(*request)));
        close(fd);
    }
}
static int publication_retry(const struct efs_msg_publication *request,int mode,
                              struct efs_msg_publication_reply *reply)
{
    int rc=EFS_ERR_BUSY;
    for(unsigned attempt=0;attempt<40;attempt++) {
        rc=efs_client_rpc_publication(request,mode,reply);
        if(!rc)return rc;
        if(rc!=EFS_ERR_BUSY && rc!=EFS_ERR_NET && rc!=EFS_ERR_NOT_PRIMARY && rc!=EFS_ERR_NO_QUORUM)break;
        usleep(100000);
    }
    fprintf(stderr,"publication retry mode=%d rc=%d state=%u\n",mode,rc,reply->state);
    return rc;
}
static struct efs_chunk_rec upload(efs_ino_t ino,uint32_t ci,uint64_t object,const uint8_t *body)
{
    struct efs_chunk_rec rec={0};rec.ino=ino;rec.chunk_index=ci;rec.chunk_generation=object;
    rec.publish_flags=EFS_CHUNK_REC_F_FRESH_OBJECT;
    uint8_t *parts[3];for(unsigned i=0;i<3;i++){parts[i]=malloc(EFS_MIN_CHUNK_SIZE/2);assert(parts[i]);}
    assert(!efs_encode_chunk(body,EFS_MIN_CHUNK_SIZE,EFS_MIN_CHUNK_SIZE,parts));
    for(unsigned i=0;i<3;i++) {
        rec.nodes[i]=i+1;efs_hash(parts[i],EFS_MIN_CHUNK_SIZE/2,rec.checksums[i]);
        size_t bytes=sizeof(struct efs_msg_put_chunk)+EFS_MIN_CHUNK_SIZE/2;
        struct efs_msg_put_chunk *put=calloc(1,bytes);assert(put);
        put->export_id=1;put->ino=ino;put->chunk_index=ci;put->fragment_index=i;
        put->chunk_generation=object;put->data_len=EFS_MIN_CHUNK_SIZE/2;put->path_hint=EFS_PATH_HINT_NEW;
        memcpy(put->checksum,rec.checksums[i],EFS_HASH_SIZE);memcpy(put+1,parts[i],put->data_len);
        int fd=connect_node(i);assert(!efs_send_msg(fd,EFS_MSG_PUT_CHUNK,put,bytes));
        void *reply=NULL;uint8_t type;uint32_t length;
        assert(!efs_recv_msg(fd,&type,&reply,&length));assert(type==EFS_MSG_PUT_CHUNK_REPLY && length>=1 && *(uint8_t *)reply==EFS_PUT_CHUNK_OK);
        free(reply);close(fd);free(put);free(parts[i]);
    }
    return rec;
}
static void base_view(efs_ino_t ino,uint64_t generation,uint32_t ci,
                      struct efs_msg_lane_writer_view_reply *view,
                      struct efs_msg_inode_getchunks_reply *mapping)
{
    int admission_rc=efs_client_rpc_writer_admission_view(1,ino,generation,ci,EFS_MIN_CHUNK_SIZE,view);
    if(admission_rc)fprintf(stderr,"typed admission ino=%llu gen=%llu ci=%u rc=%d\n",(unsigned long long)ino,(unsigned long long)generation,ci,admission_rc);
    assert(!admission_rc);
    memset(mapping,0,sizeof(*mapping));mapping->ino=ino;mapping->generation=generation;
    mapping->authority_epoch=view->view.authority_epoch;mapping->count=1;
    uint64_t discovered=0;
    assert(!efs_client_rpc_getchunks_fileid(1,ino,generation,ci,mapping->recs,&mapping->count,&discovered) && discovered==generation);
    if(mapping->count && mapping->recs[0].chunk_index==ci)
        assert(mapping->recs[0].read_view.fence_epoch==view->view.authority_epoch);
}
int main(int argc,char **argv)
{
    assert(argc==4 || argc==5);base_port=atoi(argv[1]);efs_ino_t ino=strtoull(argv[2],NULL,10);
    g_client.export_id=1;g_client.node_count=4;efs_export_init(&g_client.export,1,"default");
    pthread_mutex_init(&g_client.idx_mu,NULL);
    for(unsigned i=0;i<4;i++){g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=base_port+i;}
    efs_client_conn_init();
    if(argc==5) {
        struct efs_msg_publication saved[2];FILE *file=fopen(argv[3],"rb");
        assert(file && fread(saved,sizeof(saved),1,file)==1);fclose(file);
        for(unsigned i=0;i<2;i++) {
            struct efs_msg_publication_reply result;
            assert(!publication_retry(&saved[i],1,&result) && result.state==EFS_PUBLICATION_RETIRED);
            assert(!publication_retry(&saved[i],0,&result) && result.state==EFS_PUBLICATION_RETIRED);
        }
        struct efs_chunk_rec rec;uint32_t n=1;uint64_t gen=0;
        assert(!efs_client_rpc_getchunks_fileid(1,ino,saved[1].rec.file_generation,saved[1].rec.chunk_index,&rec,&n,&gen));
        assert(n==1 && gen==saved[1].rec.file_generation && rec.chunk_generation==saved[1].rec.chunk_generation);
        puts("cold retired exact replay cannot resurrect superseded mapping PASS");
        if(!strcmp(argv[4],"receipt-only"))return 0;
        uint32_t ci=saved[1].rec.chunk_index;
        struct efs_msg_lane_writer_view_reply view;
        struct efs_msg_inode_getchunks_reply *base=calloc(1,sizeof(*base));assert(base);
        base_view(ino,gen,ci,&view,base);
        uint8_t patch[4096];memset(patch,0x11,sizeof(patch));
        struct efs_publication_ack_queue *read_queue=NULL;
        assert(!efs_client_publication_admit_write(&read_queue,ino,gen,ci,&view,4096,patch,sizeof(patch)));
        uint8_t *image=calloc(1,EFS_MIN_CHUNK_SIZE);assert(image);
        assert(!efs_dcache_overlay_lane(ino,gen,ci,&view,base,image,EFS_MIN_CHUNK_SIZE));
        assert(!memcmp(image+4096,patch,sizeof(patch)));
        assert(efs_dcache_retire_clipped(ino,gen,ci,&view,base)==EFS_ERR_BUSY);
        struct efs_inode out;
        assert(!efs_client_rpc_setattr(1,ino,EFS_SETATTR_SIZE,0,0,0,(uint64_t)ci*EFS_MIN_CHUNK_SIZE+10,0,0,0,0,&out));
        base_view(ino,gen,ci,&view,base);
        memset(image,0,EFS_MIN_CHUNK_SIZE);memset(image,0x5a,10);
        assert(!efs_dcache_overlay_lane(ino,gen,ci,&view,base,image,EFS_MIN_CHUNK_SIZE));
        for(unsigned i=0;i<EFS_MIN_CHUNK_SIZE;i++)assert(image[i]==(i<10?0x5a:0));
        assert(!efs_dcache_retire_clipped(ino,gen,ci,&view,base));
        assert(!efs_dcache_pressure_ino(0));efs_dcache_drop(ino,ci);
        assert(!efs_client_publication_ack_free(read_queue));
        free(image);free(base);
        puts("actual truncate fence prevents typed dirty read resurrection and retires fully clipped ownership PASS");return 0;
    }
    uint32_t count=0;uint64_t generation=0;
    assert(!efs_client_rpc_getchunks_fileid(1,ino,0,0,NULL,&count,&generation) && generation);
    uint32_t ci=1;
    int same_group=getenv("EFS_TYPED_SAME_GROUP")!=NULL;
    while((efs_raft_shard_group(efs_kv_lane_shard(ino,ci%EFS_META_LANES))==
           efs_raft_shard_group(efs_kv_inode_shard(ino)))!=same_group)ci++;
    assert(ci<EFS_META_LANES);
    struct efs_msg_lane_writer_view_reply view;
    struct efs_msg_inode_getchunks_reply *mapping=calloc(1,sizeof(*mapping));assert(mapping);
    base_view(ino,generation,ci,&view,mapping);assert(!mapping->count && !view.view.authority_epoch);
    uint8_t *body=malloc(EFS_MIN_CHUNK_SIZE),*owned=malloc(EFS_MIN_CHUNK_SIZE),*image=malloc(EFS_MIN_CHUNK_SIZE),*peer=malloc(EFS_MIN_CHUNK_SIZE);
    assert(body && owned && image && peer);memset(body,0x5a,EFS_MIN_CHUNK_SIZE);
    struct efs_publication_ack_queue *queue=NULL;
    assert(!efs_client_publication_admit_write(&queue,ino,generation,ci,&view,0,body,EFS_MIN_CHUNK_SIZE));
    assert(queue && queue->bound);
    struct efs_writer_plan plan;
    assert(!efs_dcache_snapshot_publication(ino,generation,ci,&view,mapping,&plan,owned,EFS_MIN_CHUNK_SIZE));
    uint64_t object;FILE *random=fopen("/dev/urandom","rb");assert(random && fread(&object,8,1,random)==1);fclose(random);
    object=(object & (UINT64_MAX>>1))|1;uint64_t sequence=plan.cache_sequence;
    assert(!efs_dcache_begin_publication(ino,generation,ci,&plan,object));
    struct efs_chunk_rec put=upload(ino,ci,object,owned);
    assert(!efs_dcache_finish_publication(ino,generation,ci,object,sequence,EFS_OK,&put));
    assert(queue);
    struct efs_msg_publication original;
    uint64_t size=((uint64_t)ci+1)*EFS_MIN_CHUNK_SIZE;
    assert(!efs_dcache_publication_request(ino,generation,ci,object,sequence,size,&original));
    assert(!efs_publication_ack_admit(queue,&original));
    assert(!efs_client_rpc_publication_session(efs_kv_lane_shard(ino,ci%EFS_META_LANES),&original.id));
    drop_reply(&original,0);
    uint8_t patch[4096];memset(patch,0xa7,sizeof(patch));
    assert(!efs_client_publication_admit_write(&queue,ino,generation,ci,&view,4096,patch,sizeof(patch)));
    int rc=EFS_ERR_BUSY;
    for(unsigned attempt=0;attempt<100;attempt++) {
        rc=efs_client_publication_writeback(queue,ino,generation,ci,object,sequence,size+999);
        if(rc==EFS_OK)break;
        assert(rc==EFS_ERR_BUSY || rc==EFS_ERR_NET || rc==EFS_ERR_NOT_PRIMARY || rc==EFS_ERR_NO_QUORUM);usleep(100000);
    }
    assert(!rc && queue->count==1 && queue->entries[queue->head].consumed_state==EFS_PUBLICATION_COMMITTED);
    assert(efs_client_publication_ack_free(queue)==EFS_ERR_BUSY);
    struct efs_msg_publication retained;assert(!efs_publication_ack_request(queue,sequence,&retained));
    assert(!memcmp(&retained,&original,sizeof(original)) && retained.size==size);
    struct efs_msg_publication_reply committed;
    assert(!efs_client_publication_progress(queue,sequence,&committed));
    assert(committed.state==EFS_PUBLICATION_COMMITTED && committed.receipt_index && committed.receipt_term);
    /* Publish a peer's change outside the newer local4KiB range. */
    memcpy(peer,body,EFS_MIN_CHUNK_SIZE);memset(peer+8192,0x33,4096);
    struct efs_chunk_rec winner=upload(ino,ci,object+2,peer);
    winner.file_generation=generation;winner.publish_epoch=view.view.authority_epoch;winner.base_gen=object;
    winner.publish_flags|=EFS_CHUNK_REC_F_CAPTURED_FILEID|EFS_CHUNK_REC_F_CAPTURED_EPOCH;
    struct efs_ino_size_rec inode_size={.ino=ino,.size=size};
    assert(!efs_client_rpc_report_dirty_raft(1,&winner,1,&inode_size,1,1));
    struct efs_msg_publication_reply replay;
    assert(!efs_client_rpc_publication(&original,0,&replay));
    assert(replay.state==EFS_PUBLICATION_COMMITTED && replay.receipt_index==committed.receipt_index && replay.receipt_term==committed.receipt_term);
    base_view(ino,generation,ci,&view,mapping);
    assert(mapping->count==1 && mapping->recs[0].chunk_generation==object+2);
    drop_reply(&original,1); /* ACK ownership still lives independently of cache. */
    assert(!efs_dcache_snapshot_publication(ino,generation,ci,&view,mapping,&plan,owned,EFS_MIN_CHUNK_SIZE));
    assert(plan.original.count==1 && plan.original.ranges[0].off==4096 && plan.original.ranges[0].len==4096);
    assert(!efs_writer_plan_materialize(&plan,mapping,peer,owned,image,EFS_MIN_CHUNK_SIZE));
    uint8_t *read_image=malloc(EFS_MIN_CHUNK_SIZE);assert(read_image);memcpy(read_image,peer,EFS_MIN_CHUNK_SIZE);
    assert(!efs_dcache_overlay_lane(ino,generation,ci,&view,mapping,read_image,EFS_MIN_CHUNK_SIZE));
    assert(!memcmp(read_image,image,EFS_MIN_CHUNK_SIZE));free(read_image);
    assert(!memcmp(image+8192,peer+8192,4096) && !memcmp(image+4096,patch,4096));
    uint64_t next_sequence=plan.cache_sequence;
    assert(!efs_dcache_begin_publication(ino,generation,ci,&plan,object+4));
    put=upload(ino,ci,object+4,image);
    assert(!efs_dcache_finish_publication(ino,generation,ci,object+4,next_sequence,EFS_OK,&put));
    assert(!efs_client_publication_writeback(queue,ino,generation,ci,object+4,next_sequence,size));
    assert(queue->count==2 && !efs_dcache_pressure_ino(0));
    struct efs_msg_publication saved[2]={original};
    assert(!efs_publication_ack_request(queue,next_sequence,&saved[1]));
    FILE *file=fopen(argv[3],"wb");assert(file && fwrite(saved,sizeof(saved),1,file)==1 && !fclose(file));
    assert(!efs_client_publication_retire(queue) && queue->count==1);
    assert(!efs_client_publication_retire(queue) && !queue->count);
    assert(!efs_client_publication_ack_free(queue));efs_dcache_drop(ino,ci);
    printf("typed cache lost publication/retirement ACK, exact newer-range ownership and peer preservation PASS ci=%u group=%u\n",ci,efs_raft_shard_group(efs_kv_lane_shard(ino,ci%EFS_META_LANES)));
    free(body);free(owned);free(image);free(peer);free(mapping);return 0;
}
