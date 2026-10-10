#include "server_internal.h"
#include "efs/put_upload.h"
#include "efs/store.h"
#include "efs/checksum.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include "efs/raft.h"
#include <string.h>

/* Ticket authority is read before taking the local write guard. Holding the
 * guard over a metadata RPC can deadlock a Raft apply waiting to seal it.
 * A concurrently completed revocation is caught by the durable floor before
 * storage; a seal in progress waits for this guarded store to finish. */
int server_put_upload(struct efsd_server *s,const uint8_t *body,uint32_t bytes,
                       uint8_t *path_used)
{
    if(path_used)*path_used=0xff;
    if(!s || !body || !path_used || bytes<EFS_PUT_UPLOAD_HEADER_LEN)return EFS_ERR_INVAL;
    uint32_t export_id=efs_put_upload_rd32(body),fi=efs_put_upload_rd32(body+4);
    struct efs_put_ticket ticket;
    int rc=efs_put_ticket_decode(body+8,EFS_PUT_TICKET_WIRE_LEN,&ticket);
    /* Tickets currently belong to the one authoritative namespace export.
     * Do not permit a ticket to be replayed into a caller-selected export. */
    if(rc!=EFS_OK || export_id!=1 || fi>=EFS_NUM_FRAGMENTS)return EFS_ERR_INVAL;
    if(efs_ino_is_meta_table(ticket.ino) || ticket.nodes[fi]!=s->id ||
       !efs_chunk_size_valid(ticket.fragment_len*2u) ||
       bytes-EFS_PUT_UPLOAD_HEADER_LEN!=ticket.fragment_len)return EFS_ERR_INVAL;
    uint8_t checksum[EFS_HASH_SIZE];
    const uint8_t *data=body+EFS_PUT_UPLOAD_HEADER_LEN;
    efs_hash(data,ticket.fragment_len,checksum);
    if(memcmp(checksum,ticket.checksums[fi],EFS_HASH_SIZE))return EFS_ERR_PROTO;

    uint8_t query[2+EFS_PUT_TICKET_WIRE_LEN];
    query[0]=efs_raft_shard_group(efs_kv_lane_shard(ticket.ino,ticket.ci%EFS_META_LANES));
    query[1]=EFS_MD_REQ_PUT_TICKET_STATUS;memcpy(query+2,body+8,EFS_PUT_TICKET_WIRE_LEN);
    struct efs_msg_raft_mkfs_reply authority;
    server_raft_host_submit(query,sizeof(query),&authority);
    if(authority.rc!=EFS_OK)return authority.rc;
    if(authority.salt!=EFS_PUT_TICKET_ADMITTED)return EFS_ERR_STALE;

    struct efs_msg_inode_writer_view file={ticket.ino,ticket.inode_gen,ticket.ci,0};
    struct efs_msg_inode_writer_view_reply inode;
    server_raft_host_writer_view(&file,&inode);
    if(inode.status==EFS_INODE_RPC_NOT_PRIMARY)return EFS_ERR_NOT_PRIMARY;
    if(inode.status==EFS_INODE_RPC_BUSY)return EFS_ERR_BUSY;
    if(inode.status==EFS_INODE_RPC_NOT_FOUND || inode.status==EFS_INODE_RPC_STALE)return EFS_ERR_STALE;
    if(inode.status!=EFS_INODE_RPC_OK)return EFS_ERR_IO;
    if(inode.ino!=ticket.ino || inode.generation!=ticket.inode_gen)return EFS_ERR_STALE;
    if(efs_writer_view_reply_valid(&file,&inode)!=EFS_OK)return EFS_ERR_PROTO;

    struct efs_put_storage_guard guard={0};
    rc=server_put_session_begin(s,ticket.id.client_uuid,ticket.id.session_epoch,&guard);
    if(rc!=EFS_OK)return rc;
    pthread_mutex_lock(&s->lock);
    struct efs_export *ex=server_export_acquire_or_create_locked(s,export_id);
    /* Geometry comes from export configuration, never untrusted upload traffic. */
    pthread_mutex_unlock(&s->lock);
    if(!ex)rc=EFS_ERR_NOMEM;
    else if(server_frag_len(ex,ticket.ino)!=ticket.fragment_len)rc=EFS_ERR_INVAL;
    else {
        struct efs_store store;struct efs_nvme_store context;
        struct efs_frag_id fid={.export_id=export_id,.ino=ticket.ino,
            .chunk_index=ticket.ci,.fragment_index=fi,.chunk_generation=ticket.object,
            .inode_generation=ticket.inode_gen,.coding_profile_id=ticket.coding_profile};
        efs_store_nvme_bind(&store,&context,s,ex);
        efs_tls_path_used=-1;efs_tls_path_hint=-1;
        rc=efs_store_put(&store,&fid,data,ticket.fragment_len,ticket.checksums[fi]);
        efs_tls_path_hint=-1;
        if(rc==EFS_OK && efs_tls_path_used>=0 && efs_tls_path_used<256)
            *path_used=(uint8_t)efs_tls_path_used;
        efs_tls_path_used=-1;
    }
    server_export_put(s,ex);
    server_put_session_end(&guard);
    return rc;
}
