#include "server_internal.h"
#include "efs/packed_upload.h"
#include "efs/checksum.h"
#include "efs/meta_cmd.h"
#include <stdlib.h>
#include <string.h>
/* Immutable admission and FileID authority precede the local storage guard.
 * A concurrent revocation is caught by the durable session floor under that
 * guard, and the exact object fence excludes PUT versus physical deletion. */
int server_packed_upload(struct efsd_server *s, const uint8_t *body, uint32_t bytes)
{
    if (!s || !body || bytes < EFS_PACKED_PUT_HEADER) return EFS_ERR_INVAL;
    uint32_t eid = efs_put_upload_rd32(body), fi = efs_put_upload_rd32(body + 4), len = efs_put_upload_rd32(body + 8);
    if (eid != 1 || fi >= EFS_NUM_FRAGMENTS || len > EFS_PACK_UPLOAD_MAX || len > bytes - EFS_PACKED_PUT_HEADER) return EFS_ERR_INVAL;
    struct efs_pack_object object; struct efs_pack_ref refs[EFS_PACK_MAX_RECORDS]; uint32_t count;
    int rc = efs_pack_upload_decode(body + EFS_PACKED_PUT_HEADER, len, &object, refs, EFS_PACK_MAX_RECORDS, &count);
    if (rc) return rc;
    uint32_t fragment_len = object.ledger.geometry / 2;
    if (object.nodes[fi] != s->id || bytes - EFS_PACKED_PUT_HEADER - len != fragment_len) return EFS_ERR_INVAL;
    const uint8_t *data = body + EFS_PACKED_PUT_HEADER + len; uint8_t sum[EFS_HASH_SIZE];
    efs_hash(data, fragment_len, sum);
    if (memcmp(sum, object.checksums[fi], sizeof(sum))) return EFS_ERR_CHECKSUM;
    uint8_t *query = malloc(2u + len); if (!query) return EFS_ERR_NOMEM;
    query[0] = object.ledger.publication_group; query[1] = EFS_MD_REQ_PACK_STATUS;
    memcpy(query + 2, body + EFS_PACKED_PUT_HEADER, len);
    struct efs_msg_raft_mkfs_reply authority; server_raft_host_submit(query, len + 2, &authority); free(query);
    if (authority.rc) return authority.rc;
    if (authority.salt != EFS_PACK_ACTIVE) return EFS_ERR_STALE;
    for (uint32_t i = 0; i < count; i++) {
        if (efs_ino_is_meta_table(refs[i].ino)) return EFS_ERR_INVAL;
        struct efs_msg_inode_writer_view req = {refs[i].ino, refs[i].inode_generation, 0, 0};
        struct efs_msg_inode_writer_view_reply reply; server_raft_host_writer_view(&req, &reply);
        if (reply.status == EFS_INODE_RPC_NOT_PRIMARY) return EFS_ERR_NOT_PRIMARY;
        if (reply.status == EFS_INODE_RPC_BUSY) return EFS_ERR_BUSY;
        if (reply.status == EFS_INODE_RPC_STALE || reply.status == EFS_INODE_RPC_NOT_FOUND) return EFS_ERR_STALE;
        if (reply.status != EFS_INODE_RPC_OK) return EFS_ERR_IO;
        if (efs_writer_view_reply_valid(&req, &reply)) return EFS_ERR_PROTO;
        if (reply.authority_epoch != refs[i].content_epoch) return EFS_ERR_STALE;
    }
    struct efs_put_storage_guard guard = {0};
    rc = server_put_session_begin(s, object.ledger.owner.client_uuid, object.ledger.owner.session_epoch, &guard);
    if (rc) return rc;
    rc = server_write_packed_fragment(s, eid, object.ledger.segment_id, object.ledger.segment_generation,
        fi, data, fragment_len, object.checksums[fi]);
    server_put_session_end(&guard); return rc;
}
int server_packed_get(struct efsd_server *s, const uint8_t *request, uint32_t bytes,
    uint8_t *out, uint32_t capacity, uint32_t *written)
{
    if (written) *written = 0;
    if (!s || !request || !out || !written || bytes != EFS_PACKED_GET_HEADER || capacity < EFS_PACKED_GET_REPLY) return EFS_ERR_INVAL;
    uint32_t eid = efs_put_upload_rd32(request), fi = efs_put_upload_rd32(request + 20), geometry = efs_put_upload_rd32(request + 24);
    uint64_t id = efs_packed_rd64(request + 4), gen = efs_packed_rd64(request + 12);
    if (eid != 1 || !id || !gen || fi >= EFS_NUM_FRAGMENTS || geometry < EFS_SEGMENT_ALIGNMENT ||
        geometry > EFS_SEGMENT_MAX_SIZE || geometry % EFS_SEGMENT_ALIGNMENT || capacity - EFS_PACKED_GET_REPLY < geometry / 2) return EFS_ERR_INVAL;
    /* Storage can require O_DIRECT aligned buffers even though wire does not. */
    uint8_t *payload = NULL; if (posix_memalign((void **)&payload, 4096, geometry / 2)) return EFS_ERR_NOMEM;
    uint32_t len = geometry / 2; uint8_t sum[EFS_HASH_SIZE];
    int rc = server_read_packed_fragment(s, eid, id, gen, fi, len, payload, &len, sum);
    memset(out, 0, EFS_PACKED_GET_REPLY); efs_put_upload_wr32(out, (uint32_t)rc);
    if (!rc) { efs_put_upload_wr32(out + 4, len); memcpy(out + 8, sum, sizeof(sum)); memcpy(out + EFS_PACKED_GET_REPLY, payload, len); }
    *written = EFS_PACKED_GET_REPLY + (rc ? 0 : len); free(payload); return EFS_OK;
}
