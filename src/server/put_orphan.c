#include "server_internal.h"
#include "efs/put_upload.h"
#include "efs/put_ticket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* D31 + K13: delete one abandoned fragment of a retired REJECTED ticket on
 * this member (EFS_MSG_PUT_ORPHAN_DELETE). Order matters:
 *   1. the fragment is ours (nodes[fi]);
 *   2. our durable storage session floor is above the ticket's epoch. The
 *      floor is sealed only after every guarded PUT of older epochs left
 *      (put_fence.c), so no PUT of this ticket can land after the delete;
 *   3. the ticket's group, after a ReadIndex, still holds the orphan record
 *      behind the shard's rejection floor: the object was never published
 *      and never can be;
 *   4. the stored record names this exact object and checksum. A different
 *      body under this name is never deleted or acknowledged.
 * OK means no copy remains here, durably (absent counts). */
int server_put_orphan_delete(struct efsd_server *s, const uint8_t *body, uint32_t bytes)
{
    struct efs_put_ticket t;
    if (!s || !body || bytes != EFS_PUT_ORPHAN_DELETE_LEN)
        return EFS_ERR_INVAL;
    uint32_t export_id = efs_put_upload_rd32(body), fi = efs_put_upload_rd32(body + 4);
    int rc = efs_put_ticket_decode(body + 8, EFS_PUT_TICKET_WIRE_LEN, &t);
    if (rc != EFS_OK || export_id != 1 || fi >= EFS_NUM_FRAGMENTS)
        return EFS_ERR_INVAL;
    if (efs_ino_is_meta_table(t.ino) || t.nodes[fi] != s->id)
        return EFS_ERR_INVAL;
    uint32_t floor = 0;
    rc = server_put_session_floor(s, t.id.client_uuid, &floor);
    if (rc != EFS_OK)
        return rc;
    if (floor <= t.id.session_epoch)
        return EFS_ERR_BUSY; /* storage not fenced for this epoch yet */
    rc = server_raft_host_put_orphan_status(body + 8);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_ERR_STALE; /* no orphan: live, published or collected */
    if (rc != EFS_OK)
        return rc;
    struct efs_export ex;
    memset(&ex, 0, sizeof(ex));
    ex.id = export_id;
    uint64_t saved = efs_tls_chunk_gen;
    efs_tls_chunk_gen = t.object;
    rc = server_delete_fragment_if_sum(s, &ex, t.ino, t.ci, fi, t.checksums[fi]);
    efs_tls_chunk_gen = saved;
    if (rc == EFS_ERR_EXIST)
        rc = EFS_ERR_PROTO; /* another body under this object: keep it, say so */
    /* Gone: drop the emptied chunk directories, as superseded GC does. */
    if (rc == EFS_OK)
        rc = server_gc_superseded_delete(s, export_id, t.ino, t.ci, fi, t.object,
                                         t.checksums[fi], 1);
    if (getenv("EFS_PUT_TICKET_DBG"))
        fprintf(stderr, "put-orphan delete ino=%llu ci=%u fi=%u seq=%llu rc=%d\n",
                (unsigned long long)t.ino, t.ci, fi, (unsigned long long)t.id.seq, rc);
    return rc;
}
