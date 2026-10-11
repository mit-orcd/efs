/* Private four-server TCP gate; deliberately separate from mounted/RDMA
 * acceptance. Uses actual Raft admission/publication and real storage RPCs. */
#include "efs/packed_upload.h"
#include "efs/packed_reader.h"
#include "efs/pack_cohort.h"
#include "efs/publication.h"
#include "efs/network.h"
#include "efs/checksum.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define N 16u
static const char *hosts[] = {"172.16.223.57", "172.16.223.58", "172.16.223.59", "172.16.223.60"};
static uint16_t port = 19832;
struct fixture { struct efs_pack_object object; struct efs_pack_ref refs[N]; struct efs_msg_publication pubs[N]; };
static struct fixture fixture;
static int rpc(unsigned node, uint8_t type, const void *data, uint32_t len, uint8_t expected, void **out, uint32_t *bytes)
{
    int fd = efs_connect_tcp(hosts[node], port); if (fd < 0) return EFS_ERR_NET;
    efs_set_recv_timeout(fd, 10000); efs_set_send_timeout(fd, 10000); uint8_t got;
    int rc = efs_send_msg(fd, type, data, len) || efs_recv_msg(fd, &got, out, bytes) ? EFS_ERR_NET : got != expected ? EFS_ERR_PROTO : EFS_OK;
    close(fd); return rc;
}
static void command(uint8_t *data, uint32_t len, uint64_t expected_salt)
{
    for (unsigned round = 0; round < 12; round++) for (unsigned node = 0; node < 4; node++) {
        void *out = NULL; uint32_t bytes;
        int rc = rpc(node, EFS_MSG_RAFT_MKFS, data, len, EFS_MSG_RAFT_MKFS_REPLY, &out, &bytes);
        if (!rc && bytes == sizeof(struct efs_msg_raft_mkfs_reply)) {
            struct efs_msg_raft_mkfs_reply reply = *(struct efs_msg_raft_mkfs_reply *)out;
            fprintf(stderr,"command group=%u tag=%u node=%u rc=%d hint=%d\n",data[0],data[1],node,reply.rc,reply.leader_hint);
            free(out); if (!reply.rc) { if (expected_salt) assert(reply.salt == expected_salt); return; }
            if (reply.rc != EFS_ERR_BUSY && reply.rc != EFS_ERR_NOT_PRIMARY) { fprintf(stderr, "command tag=%u rc=%d\n", data[1], reply.rc); abort(); }
        } else { fprintf(stderr,"command transport node=%u rc=%d bytes=%u\n",node,rc,bytes); free(out); }
        usleep(20000);
    }
    assert(!"command exhausted");
}
static void retired_probe(void)
{
    uint8_t data[EFS_PACK_UPLOAD_MAX + 2]; size_t length;
    data[0] = fixture.object.ledger.publication_group; data[1] = EFS_MD_REQ_PACK_STATUS;
    assert(!efs_pack_upload_encode(&fixture.object, fixture.refs, N, data + 2, sizeof(data) - 2, &length));
    for (unsigned round = 0; round < 12; round++) for (unsigned node = 0; node < 4; node++) {
        void *raw = NULL; uint32_t bytes;
        int rc = rpc(node, EFS_MSG_RAFT_MKFS, data, length + 2, EFS_MSG_RAFT_MKFS_REPLY, &raw, &bytes);
        if (!rc && bytes == sizeof(struct efs_msg_raft_mkfs_reply)) {
            struct efs_msg_raft_mkfs_reply reply = *(struct efs_msg_raft_mkfs_reply *)raw; free(raw);
            if (reply.rc == EFS_ERR_STALE) { assert(!reply.salt); return; }
            assert(reply.rc == EFS_ERR_BUSY || reply.rc == EFS_ERR_NOT_PRIMARY || reply.rc == EFS_ERR_NO_QUORUM);
        } else free(raw);
        usleep(20000);
    }
    assert(!"retired floor not recovered");
}
static void create(unsigned i, struct efs_inode *out)
{
    struct efs_msg_inode_create req = {0}; req.parent = EFS_ROOT_INO; req.mode = S_IFREG | 0600; req.uid = req.gid = 246111;
    snprintf(req.name, sizeof(req.name), "packed-rpc-cohort-%u", i);
    for (unsigned round = 0; round < 12; round++) for (unsigned node = 0; node < 4; node++) {
        void *raw = NULL; uint32_t bytes;
        int rc = rpc(node, EFS_MSG_INODE_CREATE, &req, sizeof(req), EFS_MSG_INODE_CREATE_REPLY, &raw, &bytes);
        if (!rc && bytes == sizeof(struct efs_msg_inode_reply)) {
            struct efs_msg_inode_reply reply = *(struct efs_msg_inode_reply *)raw; free(raw);
            if (!reply.status) { *out = reply.inode; return; }
            if (reply.status == EFS_INODE_RPC_EXIST) {
                struct efs_msg_inode_lookup lookup = {0}; lookup.parent = EFS_ROOT_INO;
                memcpy(lookup.name, req.name, sizeof(lookup.name));
                raw = NULL;
                if (!rpc(node, EFS_MSG_INODE_LOOKUP, &lookup, sizeof(lookup), EFS_MSG_INODE_LOOKUP_REPLY, &raw, &bytes) && bytes == sizeof(reply)) {
                    reply = *(struct efs_msg_inode_reply *)raw; free(raw);
                    if (!reply.status) { *out = reply.inode; return; }
                } else free(raw);
            }
            fprintf(stderr, "create i=%u node=%u status=%u\n", i, node, reply.status);
            assert(reply.status == EFS_INODE_RPC_BUSY || reply.status == EFS_INODE_RPC_NOT_PRIMARY);
        } else free(raw);
    }
    assert(!"create exhausted");
}
static int fetch(void *context, efs_node_id_t node, uint64_t id, uint64_t generation,
    uint32_t fi, uint32_t geometry, uint8_t *out, uint32_t capacity)
{
    (void)context; uint8_t req[EFS_PACKED_GET_HEADER];
    efs_put_upload_wr32(req, 1); efs_packed_wr64(req + 4, id); efs_packed_wr64(req + 12, generation);
    efs_put_upload_wr32(req + 20, fi); efs_put_upload_wr32(req + 24, geometry);
    void *raw = NULL; uint32_t bytes; int rc = rpc(node - 1, EFS_MSG_GET_PACKED, req, sizeof(req), EFS_MSG_GET_PACKED_REPLY, &raw, &bytes);
    if (!rc && bytes >= EFS_PACKED_GET_REPLY) {
        uint8_t *p = raw; rc = (int32_t)efs_put_upload_rd32(p);
        if (!rc) { assert(bytes == EFS_PACKED_GET_REPLY + capacity && efs_put_upload_rd32(p + 4) == capacity); memcpy(out, p + EFS_PACKED_GET_REPLY, capacity); }
    } else if (!rc) rc = EFS_ERR_PROTO;
    free(raw); return rc;
}
static void verify(void)
{
    uint8_t output[EFS_MIN_CHUNK_SIZE];
    for (unsigned i = 0; i < N; i++) {
        struct efs_msg_inode_getchunks req = {1, fixture.refs[i].ino, 0, 1, fixture.refs[i].inode_generation};
        struct efs_chunk_rec record; int ok = 0;
        for (unsigned round = 0; !ok && round < 12; round++) for (unsigned node = 0; !ok && node < 4; node++) {
            void *raw = NULL; uint32_t len;
            if (!rpc(node, EFS_MSG_GETCHUNKS_PACKED, &req, sizeof(req), EFS_MSG_GETCHUNKS_PACKED_REPLY, &raw, &len) && len == sizeof(struct efs_msg_inode_getchunks_reply)) {
                struct efs_msg_inode_getchunks_reply *r = raw;
                if (!r->status) { assert(!efs_getchunks_reply_valid(&req, r) && r->count == 1); record = r->recs[0]; ok = 1; }
            }
            free(raw);
        }
        assert(ok && record.publish_flags & EFS_CHUNK_REC_F_PACKED_BASE);
        assert(!memcmp(&record.packed, fixture.refs + i, sizeof(record.packed)));
        assert(!efs_packed_read_base(&record.packed, record.nodes, record.checksums, fetch, NULL, output, sizeof(output)));
        for (unsigned j = 0; j < 512; j++) assert(output[j] == (uint8_t)(i + 1));
        for (unsigned j = 512; j < sizeof(output); j++) assert(!output[j]);
        int receipt = 0;
        for (unsigned node = 0; !receipt && node < 4; node++) {
            void *raw = NULL; uint32_t len;
            if (!rpc(node, EFS_MSG_PUBLICATION_STATUS, fixture.pubs + i, sizeof(fixture.pubs[i]), EFS_MSG_PUBLICATION_STATUS_REPLY, &raw, &len) && len == sizeof(struct efs_msg_publication_reply)) {
                struct efs_msg_publication_reply *r = raw;
                if (!r->rpc.status) { assert(r->state == EFS_PUBLICATION_COMMITTED && !r->verdict && r->receipt_index && r->receipt_term); receipt = 1; }
            }
            free(raw);
        }
        assert(receipt);
    }
    printf("packed RPC: %u authoritative mappings, durable publication receipts and exact zero-filled packed reads PASS\n", N); fflush(stdout);
}
int main(int argc, char **argv)
{
    assert(argc == 3); int resume = !strncmp(argv[1], "verify", 6), retire = !strcmp(argv[1], "retire");
    if (resume || retire) {
        FILE *f = fopen(argv[2], "rb"); assert(f && fread(&fixture, sizeof(fixture), 1, f) == 1); assert(!fclose(f));
        if (retire) {
            uint8_t bytes[EFS_PACK_UPLOAD_MAX + 2]; size_t length;
            bytes[0] = fixture.object.ledger.publication_group; bytes[1] = EFS_MD_CMD_PACK_UPLOAD_RETIRE;
            assert(!efs_pack_upload_encode(&fixture.object, fixture.refs, N, bytes + 2, sizeof(bytes) - 2, &length));
            command(bytes, length + 2, EFS_PACK_UPLOAD_RETIRED);
            command(bytes, length + 2, EFS_PACK_UPLOAD_RETIRED);
            puts("packed RPC: terminal upload permit retires idempotently while all live mappings remain readable PASS");
        }
        if (retire || !strcmp(argv[1], "verify-retired")) retired_probe();
        verify(); return 0;
    }
    struct efs_opid owner = {.client_uuid = {48}, .session_epoch = 1, .seq = 1};
    uint8_t session[23] = {0, EFS_MD_CMD_SESSION, EFS_MD_SESS_CREATE};
    session[0] = efs_raft_shard_group(efs_kv_session_shard(owner.client_uuid)); memcpy(session + 3, owner.client_uuid, 16); efs_put_upload_wr32(session + 19, 1); command(session, sizeof(session), 0);
    uint64_t id, gen; assert(!efs_pack_identity(&owner, 0, &id, &gen));
    struct efs_pack_record records[N]; uint8_t payload[N][512], image[EFS_SEGMENT_MAX_SIZE]; size_t geometry;
    struct efs_pack_cohort *cohort = malloc(sizeof(*cohort)); assert(cohort);
    assert(!efs_pack_cohort_init(cohort, 0, N, 1000000));
    for (unsigned i = 0; i < N; i++) {
        struct efs_inode inode; create(i, &inode);
        struct efs_msg_inode_writer_view req = {inode.ino, 0, 0, 0}; struct efs_msg_inode_writer_view_reply view; int ok = 0;
        for (unsigned node = 0; !ok && node < 4; node++) {
            void *raw = NULL; uint32_t len;
            if (!rpc(node, EFS_MSG_INODE_WRITER_VIEW, &req, sizeof(req), EFS_MSG_INODE_WRITER_VIEW_REPLY, &raw, &len) && len == sizeof(view)) {
                view = *(struct efs_msg_inode_writer_view_reply *)raw; ok = view.status == EFS_INODE_RPC_OK;
            } free(raw);
        }
        assert(ok && !efs_writer_view_reply_valid(&req, &view));
        struct efs_msg_publication *p = fixture.pubs + i; p->id = owner; p->id.seq = 101 + i; p->size = 512;
        p->rec.ino = inode.ino; p->rec.file_generation = view.generation; p->rec.publish_epoch = view.authority_epoch;
        p->rec.chunk_generation = efs_meta_candidate_gen(owner.client_uuid, 1, 101 + i, 0, 0);
        p->rec.publish_flags = EFS_CHUNK_REC_F_CAPTURED_EPOCH | EFS_CHUNK_REC_F_CAPTURED_FILEID | EFS_CHUNK_REC_F_FRESH_OBJECT | EFS_CHUNK_REC_F_PACKED_BASE;
        memset(payload[i], i + 1, 512); records[i] = (struct efs_pack_record){inode.ino, view.generation, view.authority_epoch, p->rec.chunk_generation, 512, payload[i]};
        assert(!efs_pack_cohort_offer(cohort, records + i, i + 1, 0));
        uint8_t establish[27] = {0, EFS_MD_CMD_SESSION, EFS_MD_SESS_ESTABLISH}; memcpy(establish + 3, owner.client_uuid, 16);
        efs_put_upload_wr32(establish + 19, 1); efs_put_upload_wr32(establish + 23, efs_kv_lane_shard(inode.ino, 0));
        establish[0] = session[0]; establish[2] = EFS_MD_SESS_REGISTER; command(establish, sizeof(establish), 0);
        establish[0] = efs_raft_shard_group(efs_kv_lane_shard(inode.ino, 0)); establish[2] = EFS_MD_SESS_ESTABLISH; command(establish, sizeof(establish), 0);
    }
    assert(efs_pack_cohort_due(cohort, 0, 0)); assert(!efs_pack_cohort_seal(cohort, &owner));
    geometry = cohort->geometry; memcpy(image, cohort->image, geometry);
    memcpy(fixture.refs, cohort->refs, sizeof(fixture.refs));
    assert(fixture.refs[0].segment_id == id && fixture.refs[0].segment_generation == gen);
    assert(!efs_pack_ledger_init(image, geometry, fixture.refs, N, &owner, 15, &fixture.object.ledger));
    uint32_t fl = geometry / 2; uint8_t *storage = malloc(3u * fl); assert(storage); uint8_t *f[] = {storage, storage + fl, storage + 2 * fl};
    assert(!efs_encode_segment(image, geometry, geometry, f));
    for (unsigned i = 0; i < 3; i++) { fixture.object.nodes[i] = i + 1; efs_hash(f[i], fl, fixture.object.checksums[i]); }
    uint8_t wire[EFS_PACK_UPLOAD_MAX], command_bytes[EFS_PACK_UPLOAD_MAX + 2]; size_t len;
    assert(!efs_pack_upload_encode(&fixture.object, fixture.refs, N, wire, sizeof(wire), &len));
    command_bytes[0] = 0; command_bytes[1] = EFS_MD_CMD_PACK_ADMIT; memcpy(command_bytes + 2, wire, len); command(command_bytes, len + 2, EFS_PACK_ACTIVE);
    uint8_t *put = malloc(EFS_PACKED_PUT_HEADER + len + fl); assert(put);
    for (unsigned i = 0; i < 3; i++) {
        efs_put_upload_wr32(put, 1); efs_put_upload_wr32(put + 4, i); efs_put_upload_wr32(put + 8, len); memcpy(put + EFS_PACKED_PUT_HEADER, wire, len); memcpy(put + EFS_PACKED_PUT_HEADER + len, f[i], fl);
        for (unsigned retry = 0; retry < 2; retry++) { void *raw = NULL; uint32_t bytes;
            assert(!rpc(i, EFS_MSG_PUT_PACKED, put, EFS_PACKED_PUT_HEADER + len + fl, EFS_MSG_PUT_PACKED_REPLY, &raw, &bytes));
            assert(bytes == 4 && !efs_put_upload_rd32(raw)); free(raw); }
    }
    for (unsigned i = 0; i < N; i++) {
        fixture.pubs[i].rec.packed = fixture.refs[i]; memcpy(fixture.pubs[i].rec.nodes, fixture.object.nodes, sizeof(fixture.object.nodes));
        memcpy(fixture.pubs[i].rec.checksums, fixture.object.checksums, sizeof(fixture.object.checksums)); int ok = 0;
        for (unsigned round = 0; !ok && round < 12; round++) for (unsigned node = 0; !ok && node < 4; node++) { void *raw = NULL; uint32_t bytes;
            if (!rpc(node, EFS_MSG_PUBLICATION, fixture.pubs + i, sizeof(fixture.pubs[i]), EFS_MSG_PUBLICATION_REPLY, &raw, &bytes) && bytes == sizeof(struct efs_msg_publication_reply)) {
                struct efs_msg_publication_reply *r = raw;
                if (!r->rpc.status) { if (r->state == EFS_PUBLICATION_COMMITTED) { assert(!r->verdict); ok = 1; } }
                else if (r->rpc.status != EFS_INODE_RPC_NOT_PRIMARY && r->rpc.status != EFS_INODE_RPC_BUSY) { fprintf(stderr,"publication i=%u node=%u status=%u verdict=%d state=%u\n",i,node,r->rpc.status,r->verdict,r->state); abort(); }
            } free(raw);
        }
        assert(ok);
    }
    FILE *save = fopen(argv[2], "wb"); assert(save && fwrite(&fixture, sizeof(fixture), 1, save) == 1); assert(!fclose(save));
    free(put); free(storage); free(cohort); verify();
}
