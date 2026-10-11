#define _XOPEN_SOURCE 700
#include "../src/server/store.c"
#include "../src/server/put_fence.c"
#include "../src/server/packed_upload.c"
#include <assert.h>
#include <ftw.h>
static struct efsd_server server;
static struct efs_pack_object expected;
static int authority_rc, race_fence;
static uint32_t phase = EFS_PACK_ACTIVE;
static uint64_t epoch;
void server_raft_host_submit(const uint8_t *p, uint32_t bytes, struct efs_msg_raft_mkfs_reply *out)
{
    assert(p[0] == expected.ledger.publication_group && p[1] == EFS_MD_REQ_PACK_STATUS);
    struct efs_pack_object object; struct efs_pack_ref refs[EFS_PACK_MAX_RECORDS]; uint32_t n;
    assert(!efs_pack_upload_decode(p + 2, bytes - 2, &object, refs, EFS_PACK_MAX_RECORDS, &n));
    assert(!memcmp(object.ledger.body_digest, expected.ledger.body_digest, EFS_HASH_SIZE));
    memset(out, 0, sizeof(*out)); out->rc = authority_rc; out->salt = phase;
    if (race_fence) assert(!server_put_session_fence(&server, expected.ledger.owner.client_uuid, 8));
}
void server_raft_host_writer_view_reuse(const struct efs_msg_inode_writer_view *q, struct efs_msg_inode_writer_view_reply *r,uint8_t *covered)
{ assert(*covered == (q->ino==101?0:1));*covered=1;memset(r, 0, sizeof(*r)); r->ino = q->ino; r->generation = q->generation; r->authority_epoch = epoch; r->oldest_complete_epoch = epoch; }
static int cleanup(const char *p, const struct stat *s, int t, struct FTW *f)
{ (void)s; (void)t; (void)f; return remove(p); }
int main(void)
{
    uint8_t payload[512] = {57}, image[EFS_SEGMENT_MAX_SIZE], wire[EFS_PACK_UPLOAD_MAX]; size_t geometry, len;
    struct efs_opid owner = {.client_uuid = {19}, .session_epoch = 7, .seq = 11};
    uint64_t id, gen; assert(!efs_pack_identity(&owner, 0, &id, &gen));
    struct efs_pack_record records[] = {{101, 3, 0, 23, 512, payload}, {103, 3, 0, 29, 512, payload}};
    struct efs_pack_ref refs[2];
    assert(!efs_pack_build(id, gen, 0, records, 2, image, sizeof(image), &geometry, refs));
    assert(!efs_pack_ledger_init(image, geometry, refs, 2, &owner, 15, &expected.ledger));
    uint32_t fl = geometry / 2; uint8_t *fragments; assert(!posix_memalign((void **)&fragments, 4096, 3u * fl));
    uint8_t *f[] = {fragments, fragments + fl, fragments + 2 * fl}; assert(!efs_encode_segment(image, geometry, geometry, f));
    for (unsigned i = 0; i < 3; i++) { expected.nodes[i] = i + 1; efs_hash(f[i], fl, expected.checksums[i]); }
    assert(!efs_pack_upload_encode(&expected, refs, 2, wire, sizeof(wire), &len));
    uint32_t bytes = EFS_PACKED_PUT_HEADER + len + fl; uint8_t *frame = malloc(bytes + 1); assert(frame);
    efs_put_upload_wr32(frame, 1); efs_put_upload_wr32(frame + 4, 0); efs_put_upload_wr32(frame + 8, len);
    memcpy(frame + EFS_PACKED_PUT_HEADER, wire, len); memcpy(frame + EFS_PACKED_PUT_HEADER + len, f[0], fl);
    for (int direct = 0; direct <= 1; direct++) {
        char root[] = "/tmp/efs-packed-upload-XXXXXX"; assert(mkdtemp(root));
        memset(&server, 0, sizeof(server)); server.id = 1; server.node_count = 1; server.nodes[0].id = 1;
        server.direct_io = direct; server.storage_path_count = 1; strcpy(server.storage_paths[0], root); strcpy(server.storage_path, root);
        pthread_mutex_init(&server.lock, NULL); assert(!server_storage_engine_init(&server));
        assert(server_packed_upload(&server, frame, bytes - 1) == EFS_ERR_INVAL);
        assert(server_packed_upload(&server, frame, bytes + 1) == EFS_ERR_INVAL);
        authority_rc = EFS_ERR_BUSY; assert(server_packed_upload(&server, frame, bytes) == EFS_ERR_BUSY); authority_rc = 0;
        phase = EFS_PACK_RECLAIMING; assert(server_packed_upload(&server, frame, bytes) == EFS_ERR_STALE); phase = EFS_PACK_ACTIVE;
        epoch = 1; assert(server_packed_upload(&server, frame, bytes) == EFS_ERR_STALE); epoch = 0;
        frame[bytes - 1] ^= 1; assert(server_packed_upload(&server, frame, bytes) == EFS_ERR_CHECKSUM); frame[bytes - 1] ^= 1;
        assert(!server_packed_upload(&server, frame, bytes)); assert(!server_packed_upload(&server, frame, bytes));
        assert(server.nodes[0].used == fl);
        uint8_t req[EFS_PACKED_GET_HEADER], reply[EFS_PACKED_GET_REPLY + EFS_SEGMENT_MAX_SIZE / 2]; uint32_t got;
        efs_put_upload_wr32(req, 1); efs_packed_wr64(req + 4, id); efs_packed_wr64(req + 12, gen);
        efs_put_upload_wr32(req + 20, 0); efs_put_upload_wr32(req + 24, geometry);
        assert(!server_packed_get(&server, req, sizeof(req), reply, sizeof(reply), &got));
        assert(got == EFS_PACKED_GET_REPLY + fl && !efs_put_upload_rd32(reply) && efs_put_upload_rd32(reply + 4) == fl);
        assert(!memcmp(reply + 8, expected.checksums[0], EFS_HASH_SIZE) && !memcmp(reply + EFS_PACKED_GET_REPLY, f[0], fl));
        race_fence = 1; assert(server_packed_upload(&server, frame, bytes) == EFS_ERR_STALE); race_fence = 0;
        assert(server.nodes[0].used == fl); assert(!server_fence_packed_object(&server, 1, id, gen));
        assert(!server_delete_packed_fragment(&server, 1, id, gen, 0, fl, expected.checksums[0]));
        assert(!server_packed_get(&server, req, sizeof(req), reply, sizeof(reply), &got));
        assert(got == EFS_PACKED_GET_REPLY && (int32_t)efs_put_upload_rd32(reply) == EFS_ERR_NOT_FOUND);
        server_storage_engine_close(&server); pthread_mutex_destroy(&server.lock);
        assert(!nftw(root, cleanup, 32, FTW_DEPTH | FTW_PHYS));
    }
    free(frame); free(fragments);
    puts("packed upload: actual buffered/direct storage, exact frame, authority/epoch/checksum gates, lost ACK, revocation race and GET/GC PASS");
}
