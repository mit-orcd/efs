#include "server_internal.h"
#include "efs/put_upload.h"
#include "efs/store.h"
#include "efs/checksum.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include "efs/raft.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The one data PUT path (K13): every fragment names an ADMITTED ticket.
 * Ticket authority is read before taking the local write guard. Holding the
 * guard over a metadata RPC can deadlock a Raft apply waiting to seal it.
 * A concurrently completed revocation is caught by the durable floor before
 * storage; a seal in progress waits for this guarded store to finish. */
static int put_upload(struct efsd_server *s,const uint8_t *body,uint32_t bytes,
                      uint8_t *path_used);
int server_put_upload(struct efsd_server *s,const uint8_t *body,uint32_t bytes,
                       uint8_t *path_used)
{
    int rc=put_upload(s,body,bytes,path_used);
    static int debug=-1;
    if(debug<0)debug=getenv("EFS_PUT_TICKET_DBG")!=NULL;
    if(rc!=EFS_OK && debug && bytes>=EFS_PUT_UPLOAD_HEADER_LEN)
        fprintf(stderr,"put-upload refused fi=%u seq=%llu rc=%d\n",efs_put_upload_rd32(body+4),
                (unsigned long long)(((uint64_t)efs_put_upload_rd32(body+28)<<32)|efs_put_upload_rd32(body+32)),rc);
    return rc;
}
static int put_upload(struct efsd_server *s,const uint8_t *body,uint32_t bytes,
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
    if(authority.rc!=EFS_OK && getenv("EFS_PUT_TICKET_DBG"))
        fprintf(stderr,"put-upload status rc=%d hint=%d group=%u\n",authority.rc,
                authority.leader_hint,query[0]);
    if(authority.rc!=EFS_OK)return authority.rc;
    if(authority.salt!=EFS_PUT_TICKET_ADMITTED)return EFS_ERR_STALE;

    /* The ADMITTED ticket under a fresh ReadIndex is the storage authority.
     * Its FileID is enforced at publication; a deleted file's ticket settles
     * GONE and its body joins the orphan ledger. No typed-writer lane view
     * here: on the REPORT path it is BUSY until REPORT bootstraps the lane,
     * which refused fragments and left chunks with two of three (K13). */

    struct efs_put_storage_guard guard={0};
    rc=server_put_session_begin(s,ticket.id.client_uuid,ticket.id.session_epoch,&guard);
    if(rc!=EFS_OK)return rc;
    pthread_mutex_lock(&s->lock);
    struct efs_export *ex=server_export_acquire_or_create_locked(s,export_id);
    /* A storage node learns the data geometry from its first PUT, here
     * only from a ticket that is ADMITTED under a live session. */
    if(ex && (ex->chunk_size==0 || ex->chunk_size==EFS_DEFAULT_CHUNK_SIZE))
        ex->chunk_size=ticket.fragment_len*2u;
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
