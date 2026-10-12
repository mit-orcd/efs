#include "server_internal.h"
#include "efs/repair_job.h"
#include "efs/checksum.h"
#include "efs/meta_cmd.h"
#include "efs/raft.h"
#include <string.h>

/* Inactive receiver prerequisite: no handler/RPC/worker selects this path.
 * Authority RPCs run before the storage guard to avoid Raft seal deadlocks. */
int server_repair_receive(struct efsd_server *s,const uint8_t *body,uint32_t bytes,
                           int seal,uint8_t *receipt)
{
    if(!receipt)return EFS_ERR_INVAL;
    memset(receipt,0,EFS_REPAIR_RECEIPT_BYTES);
    if(!s || !body || bytes<4+EFS_REPAIR_JOB_BYTES || (seal!=0 && seal!=1))return EFS_ERR_INVAL;
    if(!s->repair_experiment)return EFS_ERR_ACCES;
    uint32_t eid=((uint32_t)body[0]<<24)|((uint32_t)body[1]<<16)|((uint32_t)body[2]<<8)|body[3];
    struct efs_repair_job j;int rc=efs_repair_job_decode(body+4,EFS_REPAIR_JOB_BYTES,&j);
    if(rc)return rc;
    if(eid!=1 || j.state!=EFS_REPAIR_ADMITTED || j.flags || j.nodes[j.fi]!=s->id ||
       j.span_len || j.span_off || s->storage_engine || s->storage_path_count!=1 ||
       bytes!=4+EFS_REPAIR_JOB_BYTES+(seal?0:j.fragment_len))return EFS_ERR_INVAL;
    rc=server_repair_store_matches(s,j.target_store);if(rc)return rc;
    const uint8_t *data=body+4+EFS_REPAIR_JOB_BYTES;
    if(!seal){uint8_t hash[32];efs_hash(data,j.fragment_len,hash);
        if(memcmp(hash,j.sums[j.fi],32))return EFS_ERR_CHECKSUM;}
    uint8_t query[2+EFS_REPAIR_JOB_BYTES];
    query[0]=efs_raft_shard_group(efs_kv_lane_shard(j.ino,j.ci%EFS_META_LANES));
    query[1]=seal?EFS_MD_REQ_REPAIR_SEAL_AUTHORITY:EFS_MD_REQ_REPAIR_WRITE_AUTHORITY;
    memcpy(query+2,body+4,EFS_REPAIR_JOB_BYTES);
    struct efs_msg_raft_mkfs_reply authority;server_raft_host_submit(query,sizeof(query),&authority);
    if(authority.rc)return authority.rc;
    if(!seal){
        struct efs_msg_inode_writer_view file={j.ino,j.inode_gen,j.ci,0};
        struct efs_msg_inode_writer_view_reply inode;server_raft_host_writer_view(&file,&inode);
        if(inode.status==EFS_INODE_RPC_NOT_PRIMARY)return EFS_ERR_NOT_PRIMARY;
        if(inode.status==EFS_INODE_RPC_BUSY)return EFS_ERR_BUSY;
        if(inode.status==EFS_INODE_RPC_NOT_FOUND || inode.status==EFS_INODE_RPC_STALE)return EFS_ERR_STALE;
        if(inode.status!=EFS_INODE_RPC_OK)return EFS_ERR_IO;
        if(inode.ino!=j.ino || inode.generation!=j.inode_gen)return EFS_ERR_STALE;
        if(efs_writer_view_reply_valid(&file,&inode))return EFS_ERR_PROTO;
    }
    struct efs_put_storage_guard guard={0};
    if(!seal){rc=server_repair_storage_begin(s,j.uuid,j.epoch,&guard);if(rc)return rc;}
    rc=server_repair_store_matches(s,j.target_store);
    if(!rc && seal)rc=server_repair_storage_seal(s,j.uuid,j.epoch+1u);
    if(!rc && !seal){
        pthread_mutex_lock(&s->lock);struct efs_export *ex=server_export_acquire_or_create_locked(s,eid);pthread_mutex_unlock(&s->lock);
        if(!ex)rc=EFS_ERR_NOMEM;
        else if(!efs_chunk_size_valid(ex->chunk_size) || server_frag_len(ex,j.ino)!=j.fragment_len)rc=EFS_ERR_INVAL;
        else rc=server_repair_file_atomic(s,ex,j.ino,j.ci,j.fi,j.object,data,j.fragment_len,j.sums[j.fi]);
        server_export_put(s,ex);
    }
    server_put_session_end(&guard);
    if(!rc)rc=efs_repair_receipt_encode(&j,seal?EFS_REPAIR_RECEIPT_SEAL:EFS_REPAIR_RECEIPT_DATA,receipt);
    return rc;
}

static int repair_inode_gc_authority(void *context,efs_ino_t ino)
{
    (void)context;return server_raft_host_repair_gc_inode_local_ready(ino);
}
int server_repair_gc_inode(struct efsd_server *s,efs_export_id_t eid,efs_ino_t ino,uint64_t generation)
{
    if(!s || !s->repair_experiment)return EFS_ERR_ACCES;
    /* ReadIndex may wait for session-fence apply. Never await it while holding
     * the inode guard: PUT holds a session stripe before taking inode RD. */
    int rc=server_raft_host_repair_gc_ready(ino,0,0);
    if(rc)return rc;
    return server_gc_inode_checked(s,eid,ino,generation,repair_inode_gc_authority,s);
}
