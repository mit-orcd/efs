/* Explicit experimental manual repair. Clients select identity/role only;
 * the coordinator captures authority, reads both survivors and verifies all
 * three authoritative digests. No caller bytes or receipts are accepted. */
#include "server_internal.h"
#include "efs/repair_job.h"
#include "efs/checksum.h"
#include "efs/network.h"
#include "efs/put_upload.h"
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

static pthread_mutex_t repair_worker_mu=PTHREAD_MUTEX_INITIALIZER;

static int repair_rpc(struct efsd_server *s,uint32_t node,uint8_t type,
                      const void *body,uint32_t bytes,void **out,uint32_t *len)
{
    char addr[64]={0};uint16_t port=0;
    pthread_mutex_lock(&s->lock);
    for(uint32_t i=0;i<s->node_count;i++)if(s->nodes[i].id==node){
        memcpy(addr,s->nodes[i].addr,sizeof(addr));port=s->nodes[i].port;break;
    }
    pthread_mutex_unlock(&s->lock);
    if(!port)return EFS_ERR_IO;
    /* A cold peer may be alive while the shared control pool still has a
     * cached down-cooldown. Manual repair owns one bounded worker and uses
     * a fresh connection, like the dedicated consensus sender. */
    struct efs_conn *c=server_peer_conn_new(addr,port);if(!c)return EFS_ERR_IO;
    uint8_t got=0;int rc=EFS_ERR_IO;*out=NULL;*len=0;
    int sent=efs_conn_send_msg(c,type,body,bytes);
    int received=sent?-1:efs_conn_recv_msg(c,&got,out,len);
    if(!sent && !received && got==type+1)rc=EFS_OK;
    if(rc)fprintf(stderr,"Repair peer: node=%u type=%u send=%d recv=%d reply=%u bytes=%u kind=%d errno=%d\n",
        node,type,sent,received,got,*len,c->kind,errno);
    if(rc){free(*out);*out=NULL;}
    efs_conn_destroy(c);
    return rc;
}

static void envelope(uint8_t *b)
{
    efs_put_upload_wr32(b,EFS_VERSION_PACK);
    memset(b+4,0,EFS_BUILD_ID_LEN);memcpy(b+4,EFS_BUILD_ID,strlen(EFS_BUILD_ID));
}

static int target(struct efsd_server *s,const struct efs_repair_job *j,
                  int seal,const uint8_t *data,uint8_t *receipt)
{
    uint32_t head=4+EFS_BUILD_ID_LEN+1+4+EFS_REPAIR_JOB_BYTES;
    uint32_t n=head+(seal?0:j->fragment_len);
    uint8_t *b=calloc(1,n);if(!b)return EFS_ERR_NOMEM;
    envelope(b);b[4+EFS_BUILD_ID_LEN]=seal?2:1;
    efs_put_upload_wr32(b+4+EFS_BUILD_ID_LEN+1,1);
    int rc=efs_repair_job_encode(j,b+4+EFS_BUILD_ID_LEN+5);
    if(!seal)memcpy(b+head,data,j->fragment_len);
    void *reply=NULL;uint32_t len=0;
    if(!rc)rc=repair_rpc(s,j->nodes[j->fi],EFS_MSG_REPAIR_TARGET,b,n,&reply,&len);
    free(b);
    if(!rc){
        if(len!=4+EFS_REPAIR_RECEIPT_BYTES)rc=EFS_ERR_PROTO;
        else {rc=(int32_t)efs_put_upload_rd32(reply);if(!rc)memcpy(receipt,(uint8_t *)reply+4,EFS_REPAIR_RECEIPT_BYTES);}
    }
    free(reply);return rc;
}

int server_repair_run(struct efsd_server *s,const struct efs_repair_job *selector)
{
    if(!s || !selector)return EFS_ERR_INVAL;
    if(!s->repair_experiment || s->storage_engine || s->storage_path_count!=1)
        return EFS_ERR_ACCES;
    if(pthread_mutex_trylock(&repair_worker_mu))return EFS_ERR_BUSY;
    const char *phase="capture";
    struct efs_repair_job j;uint8_t state=0,flags=0;
    int rc=server_raft_host_repair_prepare(selector,&j,&state,&flags);
    void *reply=NULL;uint32_t len=0;uint8_t env[4+EFS_BUILD_ID_LEN];
    uint8_t *parts[3]={0};uint8_t receipt[EFS_REPAIR_RECEIPT_BYTES];
    if(rc)goto done;
    envelope(env);
    phase="target identity";
    rc=repair_rpc(s,j.nodes[j.fi],EFS_MSG_REPAIR_STORE_ID,env,sizeof(env),&reply,&len);
    if(rc)goto done;
    if(len!=28+EFS_BUILD_ID_LEN || (int32_t)efs_put_upload_rd32(reply) ||
       efs_put_upload_rd32((uint8_t *)reply+4)!=1 ||
       efs_put_upload_rd32((uint8_t *)reply+8)!=j.nodes[j.fi] ||
       memcmp((uint8_t *)reply+28,env+4,EFS_BUILD_ID_LEN)){rc=EFS_ERR_PROTO;goto done;}
    if(state && memcmp(j.target_store,(uint8_t *)reply+12,16)){rc=EFS_ERR_STALE;goto done;}
    memcpy(j.target_store,(uint8_t *)reply+12,16);free(reply);reply=NULL;
    /* Re-capture binds the actual target incarnation into the canonical job. */
    phase="bound capture";
    rc=server_raft_host_repair_prepare(&j,&j,&state,&flags);if(rc)goto done;
    if(flags&EFS_REPAIR_CANCELLED){rc=EFS_ERR_STALE;goto done;}
    if(!(flags&EFS_REPAIR_DATA_DURABLE)){
        for(unsigned f=0;f<3;f++){
            parts[f]=malloc(j.fragment_len);if(!parts[f]){rc=EFS_ERR_NOMEM;goto done;}
            if(f==j.fi)continue;
            struct efs_msg_get_chunk q={.export_id=1,.ino=j.ino,.chunk_index=j.ci,
                .fragment_index=f,.chunk_generation=j.object};
            phase="source read";
            rc=repair_rpc(s,j.nodes[f],EFS_MSG_GET_CHUNK,&q,sizeof(q),&reply,&len);
            if(rc)goto done;
            if(len!=1+EFS_HASH_SIZE+j.fragment_len || ((uint8_t *)reply)[0]!=EFS_GET_CHUNK_OK ||
               memcmp((uint8_t *)reply+1,j.sums[f],EFS_HASH_SIZE)){rc=EFS_ERR_CHECKSUM;goto done;}
            memcpy(parts[f],(uint8_t *)reply+1+EFS_HASH_SIZE,j.fragment_len);
            uint8_t sum[EFS_HASH_SIZE];efs_hash(parts[f],j.fragment_len,sum);
            if(memcmp(sum,j.sums[f],sizeof(sum))){rc=EFS_ERR_CHECKSUM;goto done;}
            free(reply);reply=NULL;
        }
        unsigned a=(j.fi+1)%3,b=(j.fi+2)%3;
        for(uint32_t i=0;i<j.fragment_len;i++)parts[j.fi][i]=parts[a][i]^parts[b][i];
        uint8_t sum[EFS_HASH_SIZE];efs_hash(parts[j.fi],j.fragment_len,sum);
        if(memcmp(sum,j.sums[j.fi],sizeof(sum))){rc=EFS_ERR_CHECKSUM;goto done;}
        phase="admit";
        rc=server_raft_host_repair_record(EFS_REPAIR_OP_ADMIT,&j,0,NULL,0);if(rc)goto done;
        phase="target data";
        rc=target(s,&j,0,parts[j.fi],receipt);if(rc)goto done;
        phase="commit data receipt";
        rc=server_raft_host_repair_record(EFS_REPAIR_OP_DATA,&j,0,receipt,sizeof(receipt));if(rc)goto done;
    }
    if(state!=EFS_REPAIR_SEALED){
        phase="target seal";
        rc=target(s,&j,1,NULL,receipt);if(rc)goto done;
        phase="commit seal receipt";
        rc=server_raft_host_repair_record(EFS_REPAIR_OP_SEAL,&j,j.epoch+1u,receipt,sizeof(receipt));if(rc)goto done;
    }
    phase="retire";
    rc=server_raft_host_repair_record(EFS_REPAIR_OP_RETIRE,&j,0,NULL,0);
done:
    if(rc)fprintf(stderr,"Repair manual: ino=%llu role=%u phase=%s rc=%d; no debt is discarded\n",
        (unsigned long long)selector->ino,selector->fi,phase,rc);
    free(reply);for(unsigned f=0;f<3;f++)free(parts[f]);
    pthread_mutex_unlock(&repair_worker_mu);return rc;
}
