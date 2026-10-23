/* D31 member side: actual frame decoder, durable storage session floor and
 * delete order of server_put_orphan_delete; mock group authority and disk. */
#include "../src/server/put_fence.c"
#include "../src/server/put_orphan.c"
#include <assert.h>
#include "test_tmpdir.h"
static struct efsd_server server;
static struct efs_put_ticket expected;
static uint8_t expected_wire[EFS_PUT_TICKET_WIRE_LEN];
static int authority_rc, delete_rc, authorities, deletes, cleanups;
__thread uint64_t efs_tls_chunk_gen;
int server_raft_host_put_orphan_status(const uint8_t ticket[216])
{
    assert(!memcmp(ticket, expected_wire, sizeof(expected_wire)));
    authorities++;
    return authority_rc;
}
int server_delete_fragment_if_sum(struct efsd_server *s, struct efs_export *ex, efs_ino_t ino,
                                  uint32_t ci, uint32_t fi, const uint8_t sum[EFS_HASH_SIZE])
{
    /* The delete names exactly this ticket's object and checksum. */
    assert(s == &server && ex->id == 1 && ino == expected.ino && ci == expected.ci && fi == 0);
    assert(efs_tls_chunk_gen == expected.object && !memcmp(sum, expected.checksums[0], EFS_HASH_SIZE));
    deletes++;
    return delete_rc;
}
int server_gc_superseded_delete(struct efsd_server *s, efs_export_id_t eid, efs_ino_t ino,
                                uint32_t ci, uint32_t fi, uint64_t gen,
                                const uint8_t sum[EFS_HASH_SIZE], int durable)
{
    assert(s == &server && eid == 1 && ino == expected.ino && ci == expected.ci && fi == 0 &&
           gen == expected.object && durable && !memcmp(sum, expected.checksums[0], EFS_HASH_SIZE));
    cleanups++;
    return EFS_OK;
}
int main(void)
{
    char root[EFS_TEST_PATH];
    efs_test_path(root, sizeof(root), "efs-put-orphan-XXXXXX");
    assert(efs_test_mkdtemp(root));
    server.storage_path_count = 1;
    server.id = 1;
    strcpy(server.storage_paths[0], root);
    pthread_mutex_init(&server.lock, NULL);
    struct efs_meta_pub p = {.ino = 4097, .inode_gen = 7, .chunk_index = 3,
                             .coding_profile_id = EFS_META_PROFILE_K2F1, .fresh_object = 1,
                             .put_fragment_len = EFS_MIN_CHUNK_SIZE / 2, .put_member_mask = 7};
    p.put_id.client_uuid[0] = 5;
    p.put_id.session_epoch = 3;
    p.put_id.seq = 11;
    p.publication_id = p.put_id;
    p.candidate_gen = efs_meta_candidate_gen(p.put_id.client_uuid, 3, 11, 3, 0);
    for (unsigned i = 0; i < 3; i++) {
        p.ch.nodes[i] = i + 1;
        memset(p.ch.checksums[i], (int)(40 + i), EFS_HASH_SIZE);
    }
    assert(!efs_put_ticket_from_pub(&p, &expected));
    assert(!efs_put_ticket_encode(&expected, expected_wire));
    uint8_t frame[EFS_PUT_ORPHAN_DELETE_LEN + 1];
    efs_put_upload_wr32(frame, 1);
    efs_put_upload_wr32(frame + 4, 0);
    memcpy(frame + 8, expected_wire, sizeof(expected_wire));
    const uint32_t len = EFS_PUT_ORPHAN_DELETE_LEN;
    /* Malformed or not ours: refused before any authority or disk work. */
    assert(server_put_orphan_delete(&server, frame, len - 1) == EFS_ERR_INVAL);
    assert(server_put_orphan_delete(&server, frame, len + 1) == EFS_ERR_INVAL);
    frame[3] = 2;
    assert(server_put_orphan_delete(&server, frame, len) == EFS_ERR_INVAL);
    frame[3] = 1;
    frame[7] = 3;
    assert(server_put_orphan_delete(&server, frame, len) == EFS_ERR_INVAL);
    frame[7] = 1; /* fragment 1 lives on node 2 */
    assert(server_put_orphan_delete(&server, frame, len) == EFS_ERR_INVAL);
    frame[7] = 0;
    frame[8 + 16] ^= 0xff; /* epoch: the object name no longer derives */
    assert(server_put_orphan_delete(&server, frame, len) == EFS_ERR_INVAL);
    frame[8 + 16] ^= 0xff;
    assert(!authorities && !deletes);
    /* No storage certificate, or one not above the epoch: a guarded PUT of
     * this epoch could still land here, so nothing is deleted. */
    assert(server_put_orphan_delete(&server, frame, len) == EFS_ERR_BUSY && !authorities);
    assert(!server_put_session_fence(&server, expected.id.client_uuid, 3));
    assert(server_put_orphan_delete(&server, frame, len) == EFS_ERR_BUSY && !authorities);
    assert(!server_put_session_fence(&server, expected.id.client_uuid, 4));
    uint32_t floor = 0;
    assert(!server_put_session_floor(&server, expected.id.client_uuid, &floor) && floor == 4);
    /* Negative control: no orphan record (a published, live or collected
     * ticket) is never deleted. */
    authority_rc = EFS_ERR_NOT_FOUND;
    assert(server_put_orphan_delete(&server, frame, len) == EFS_ERR_STALE && authorities == 1 && !deletes);
    authority_rc = EFS_ERR_BUSY; /* shard floor not raised */
    assert(server_put_orphan_delete(&server, frame, len) == EFS_ERR_BUSY && !deletes);
    authority_rc = EFS_ERR_NOT_PRIMARY;
    assert(server_put_orphan_delete(&server, frame, len) == EFS_ERR_NOT_PRIMARY && !deletes);
    authority_rc = EFS_OK;
    /* Another body under this object name: kept and not acknowledged. */
    delete_rc = EFS_ERR_EXIST;
    efs_tls_chunk_gen = 99;
    assert(server_put_orphan_delete(&server, frame, len) == EFS_ERR_PROTO && deletes == 1 && !cleanups);
    assert(efs_tls_chunk_gen == 99);
    delete_rc = EFS_ERR_IO;
    assert(server_put_orphan_delete(&server, frame, len) == EFS_ERR_IO && !cleanups);
    /* Deleted (or already absent): durable, directories cleaned. Retries
     * repeat the same checks and answer OK again. */
    delete_rc = EFS_OK;
    assert(server_put_orphan_delete(&server, frame, len) == EFS_OK && cleanups == 1);
    assert(server_put_orphan_delete(&server, frame, len) == EFS_OK && cleanups == 2);
    assert(efs_tls_chunk_gen == 99);
    char cert[EFS_MAX_PATH + 80];
    pthread_mutex_t *mu;
    assert(!storage_fence_path(&server, expected.id.client_uuid, cert, &mu));
    assert(!unlink(cert));
    *strrchr(cert, '/') = 0;
    assert(!rmdir(cert));
    *strrchr(cert, '/') = 0;
    assert(!rmdir(cert));
    assert(!rmdir(root));
    puts("D31 member delete: own fragment only, refused below the storage session floor, "
         "group orphan record as authority (published/live never deleted), checksum-guarded "
         "durable delete with idempotent retry PASS");
    return 0;
}
