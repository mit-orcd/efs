/* More than32 fresh cross-group lanes in one real uploaded REPORT. */
#include "../../src/client/client_internal.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv)
{
    assert(argc==3);unsigned port=atoi(argv[1]);
    g_client.export_id=1;g_client.node_count=4;
    for(unsigned i=0;i<4;i++){g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=port+i;}
    efs_client_conn_init();
    struct efs_chunk_rec *recs=calloc(40,sizeof(*recs));assert(recs);
    struct efs_ino_size_rec inodes[40];memset(inodes,0,sizeof(inodes));
    uint8_t *body=malloc(EFS_MIN_CHUNK_SIZE);assert(body);
    uint8_t *frags[3];for(unsigned j=0;j<3;j++){frags[j]=malloc(EFS_MIN_CHUNK_SIZE/2);assert(frags[j]);}
    FILE *input=fopen(argv[2],"r");assert(input);
    for(unsigned i=0;i<40;i++) {
        unsigned long long ino;assert(fscanf(input,"%llu",&ino)==1);recs[i].ino=ino;
        uint32_t count=0;uint64_t gen=0;
        assert(!efs_client_rpc_getchunks_fileid(1,ino,0,0,NULL,&count,&gen) && gen);
        uint32_t ci=1;
        while(efs_raft_shard_group(efs_kv_lane_shard(ino,ci%EFS_META_LANES))==efs_raft_shard_group(efs_kv_inode_shard(ino)))ci++;
        assert(ci<EFS_META_LANES);recs[i].chunk_index=ci;recs[i].file_generation=gen;
        struct efs_msg_inode_writer_view_reply view;
        assert(!efs_client_rpc_writer_view(ino,gen,ci,&view));recs[i].publish_epoch=view.authority_epoch;
        recs[i].publish_flags=EFS_CHUNK_REC_F_CAPTURED_FILEID|EFS_CHUNK_REC_F_CAPTURED_EPOCH|EFS_CHUNK_REC_F_FRESH_OBJECT;
        memset(body,i+1,EFS_MIN_CHUNK_SIZE);assert(!efs_encode_chunk(body,EFS_MIN_CHUNK_SIZE,EFS_MIN_CHUNK_SIZE,frags));
        const uint8_t *parts[3];for(unsigned j=0;j<3;j++){parts[j]=frags[j];recs[i].nodes[j]=j+1;efs_hash(frags[j],EFS_MIN_CHUNK_SIZE/2,recs[i].checksums[j]);}
        assert(!efs_client_put_fragments_parallel(ino,ci,recs[i].nodes,parts,EFS_MIN_CHUNK_SIZE/2,recs[i].checksums,&recs[i].chunk_generation));
        inodes[i].ino=ino;inodes[i].size=((uint64_t)ci+1)*EFS_MIN_CHUNK_SIZE;
    }
    assert(fgetc(input)== '\n' || feof(input));fclose(input);
    /* Exercise a committed valid prefix followed by a permanent invalid rec.
     * Whole-batch success would discard the second body's dirty ownership. */
    struct efs_chunk_rec prefix[2] = { recs[0], recs[1] };
    prefix[1].nodes[0] = 0;
    assert(efs_client_rpc_report_dirty_raft(1,prefix,2,inodes,2,1)==EFS_ERR_INVAL);
    struct efs_chunk_rec accepted;
    uint32_t accepted_count = 1;
    uint64_t accepted_fileid = 0;
    assert(!efs_client_rpc_getchunks_fileid(1,recs[0].ino,recs[0].file_generation,
        recs[0].chunk_index,&accepted,&accepted_count,&accepted_fileid));
    assert(accepted_count==1 && accepted_fileid==recs[0].file_generation &&
        accepted.chunk_generation==recs[0].chunk_generation &&
        !memcmp(accepted.nodes,recs[0].nodes,sizeof(accepted.nodes)) &&
        !memcmp(accepted.checksums,recs[0].checksums,sizeof(accepted.checksums)));
    fprintf(stderr,"REPORT valid prefix plus invalid tail refuses whole-batch success PASS\n");
    assert(!efs_client_rpc_report_dirty_raft(1,recs,40,inodes,40,1));
    for(unsigned i=0;i<40;i++)printf("ino=%llu ci=%u byte=%u\n",(unsigned long long)recs[i].ino,recs[i].chunk_index,i+1);
    for(unsigned j=0;j<3;j++) { free(frags[j]); }
    free(body);free(recs);
    return 0;
}
