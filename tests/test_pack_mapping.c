#include "efs/pack_mapping.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/checksum.h"
#include "efs/metadata.h"
#include "efs/session.h"
#include "efs/raft.h"
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define N 16u
static void put(uint8_t *p, uint64_t n, unsigned bytes)
{ while (bytes) { p[--bytes] = (uint8_t)n; n >>= 8; } }
static int cleanup(const char *p, const struct stat *s, int kind, struct FTW *f)
{ (void)s; (void)kind; (void)f; return remove(p); }
static void cohort(struct efs_kv *kv, int fail_batch)
{
    assert(!efs_meta_apply_init(kv, 100));
    struct efs_pack_record records[N]; struct efs_pack_ref refs[N];
    uint8_t payload[512], image[EFS_SEGMENT_MAX_SIZE], wire[EFS_PACK_OBJECT_MAX_BYTES];
    memset(payload, 0x97, sizeof(payload));
    const struct efs_meta_attrs attrs = {1000, 1000, 100};
    for (unsigned i = 0; i < N; i++) {
        char name[32]; efs_ino_t ino; struct efs_meta_row row;
        snprintf(name, sizeof(name), "packed-%u", i);
        assert(!efs_meta_apply_create_file(kv, &attrs, EFS_ROOT_INO, S_IFREG | 0600, name, &ino));
        assert(!efs_meta_apply_get_inode(kv, ino, &row));
        records[i] = (struct efs_pack_record){ino, row.generation, 0, 19 + i, sizeof(payload), payload};
        /* Establish a real inode/lane stamp; the fixture then substitutes a
         * packed base while the planner supplies its shared ownership. */
        struct efs_meta_pub seed = {.ino = ino, .new_size = sizeof(payload),
            .now = 100, .candidate_gen = 500 + i, .coding_profile_id = EFS_META_PROFILE_K2F1};
        for (unsigned j = 0; j < EFS_NUM_FRAGMENTS; j++) seed.ch.nodes[j] = j + 1;
        assert(!efs_meta_apply_publish(kv, &seed));
    }
    uint32_t group = efs_raft_shard_group(efs_kv_lane_shard(records[0].ino, 0));
    size_t geometry, len;
    assert(!efs_pack_build(41, 43, group, records, N, image, sizeof(image), &geometry, refs));
    struct efs_pack_object object = {0};
    struct efs_opid owner = {.client_uuid = {7}, .session_epoch = 11, .seq = 13};
    assert(!efs_pack_ledger_init(image, geometry, refs, N, &owner, 15, &object.ledger));
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        object.nodes[i] = i + 1; memset(object.checksums[i], 0x41 + i, EFS_HASH_SIZE);
    }
    uint32_t anch = efs_kv_anchor_shard(efs_kv_lane_shard(records[0].ino, 0));
    assert(!efs_session_establish(kv, anch, owner.client_uuid, owner.session_epoch));
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES];
    assert(!efs_pack_object_key(group, 41, 43, key));
    assert(!efs_pack_object_encode(&object, wire, sizeof(wire), &len));
    assert(!efs_kv_put(kv, key, sizeof(key), wire, (uint32_t)len));
    uint8_t corrupt[EFS_PACK_OBJECT_MAX_BYTES]; memcpy(corrupt, wire, len); corrupt[24] ^= 1;
    struct efs_pack_object read;
    assert(efs_pack_object_decode(corrupt, len, &read) == EFS_ERR_CHECKSUM);
    struct efs_meta_chunk chunks[N]; struct efs_pack_mapping_change changes[N];
    uint8_t map_keys[N][EFS_KV_KEY_MAX], values[N][128 + EFS_PACK_REF_BYTES];
    uint32_t kl[N]; struct efs_kv_item batch[N + 2];
    for (unsigned i = 0; i < N; i++) {
        chunks[i] = (struct efs_meta_chunk){.generation = records[i].record_generation,
            .coding_profile_id = EFS_META_PROFILE_PACK_K2F1, .content_epoch = records[i].content_epoch,
            .packed = refs[i]};
        memcpy(chunks[i].nodes, object.nodes, sizeof(object.nodes));
        memcpy(chunks[i].checksums, object.checksums, sizeof(object.checksums));
        changes[i] = (struct efs_pack_mapping_change){records[i].ino, records[i].inode_generation, 0, NULL, &chunks[i]};
        assert(!efs_kv_key_chunk(efs_kv_lane_shard(records[i].ino, 0), records[i].ino,
            records[i].inode_generation, 0, 0, map_keys[i], &kl[i]));
        memset(values[i], 0, sizeof(values[i]));
        put(values[i], chunks[i].generation, 8); put(values[i] + 8, EFS_META_PROFILE_PACK_K2F1, 4);
        put(values[i] + 12, chunks[i].content_epoch, 8);
        for (unsigned j = 0; j < EFS_NUM_FRAGMENTS; j++) put(values[i] + 20 + j * 4, chunks[i].nodes[j], 4);
        memcpy(values[i] + 32, chunks[i].checksums, 96);
        assert(!efs_pack_ref_encode(&refs[i], values[i] + 128));
        batch[i] = (struct efs_kv_item){EFS_KV_PUT, map_keys[i], kl[i], values[i], sizeof(values[i])};
    }
    struct efs_pack_mapping_plan plan = {0};
    chunks[N - 1].packed.inode_generation++;
    assert(efs_pack_mapping_plan(kv, changes, N, &plan) == EFS_ERR_PROTO);
    assert(!plan.count && !plan.storage);
    chunks[N - 1].packed.inode_generation--;
    assert(!efs_pack_object_get(kv, group, 41, 43, &read));
    for (unsigned i = 0; i < N; i++) assert(read.ledger.state[i] == EFS_PACK_PENDING);
    uint8_t intent[EFS_KV_KEY_MAX]; uint32_t il;
    assert(!efs_kv_key_intent(key, sizeof(key), intent, &il));
    assert(!efs_kv_put(kv, intent, il, (uint8_t[]){1}, 1));
    assert(efs_pack_mapping_plan(kv, changes, N, &plan) == EFS_ERR_BUSY);
    assert(!efs_kv_del(kv, intent, il));
    assert(!efs_pack_mapping_plan(kv, changes, N, &plan));
    assert(plan.count == 1); /* Load/modify once; no lost updates across members. */
    uint8_t receipt_key[] = {0, 1, 250}, receipt[] = {1};
    batch[N] = (struct efs_kv_item){EFS_KV_PUT, receipt_key, sizeof(receipt_key), receipt, sizeof(receipt)};
    batch[N + 1] = plan.items[0];
    if (fail_batch) {
        assert(!efs_kv_mem_fail_next_batch(kv));
        assert(efs_kv_batch(kv, batch, N + 2) != EFS_OK);
        uint8_t probe[sizeof(values[0])]; uint32_t pl = sizeof(probe);
        struct efs_meta_chunk old; struct efs_meta_delta delta; uint32_t nd; uint64_t seq;
        assert(!efs_meta_apply_get_chunk_row(kv, records[0].ino, records[0].inode_generation,
            0, &old, &delta, 1, &nd, &seq));
        assert(old.coding_profile_id == EFS_META_PROFILE_K2F1 && old.generation == 500 && !nd);
        assert(!efs_pack_object_get(kv, group, 41, 43, &read));
        for (unsigned i = 0; i < N; i++) assert(read.ledger.state[i] == EFS_PACK_PENDING);
        pl = sizeof(probe); assert(efs_kv_get(kv, receipt_key, sizeof(receipt_key), probe, &pl) == EFS_ERR_NOT_FOUND);
    }
    assert(!efs_kv_batch(kv, batch, N + 2)); efs_pack_mapping_plan_free(&plan);
    assert(!efs_pack_object_get(kv, group, 41, 43, &read));
    for (unsigned i = 0; i < N; i++) assert(read.ledger.state[i] == EFS_PACK_LIVE);
    /* A repeat without a current before image cannot recreate old ownership. */
    assert(efs_pack_mapping_plan(kv, changes, N, &plan) == EFS_ERR_STALE);
    changes[0].before = &chunks[0];
    assert(!efs_pack_mapping_plan(kv, changes, 1, &plan)); assert(!plan.count);
    efs_pack_mapping_plan_free(&plan); /* Ordinary deltas preserve the packed base. */
    struct efs_meta_chunk wrong = chunks[0]; wrong.nodes[0] = 4; changes[0].after = &wrong;
    assert(efs_pack_mapping_plan(kv, changes, 1, &plan) == EFS_ERR_STALE);
    changes[0].after = NULL;
    assert(!efs_pack_mapping_plan(kv, changes, 1, &plan)); assert(plan.count == 1);
    struct efs_kv_item del[2] = {{EFS_KV_DEL, map_keys[0], kl[0], NULL, 0}, plan.items[0]};
    assert(!efs_kv_batch(kv, del, 2)); efs_pack_mapping_plan_free(&plan);
    assert(!efs_pack_object_get(kv, group, 41, 43, &read));
    assert(read.ledger.state[0] == EFS_PACK_RELEASED && read.ledger.phase == EFS_PACK_ACTIVE);
    for (unsigned i = 1; i < N; i++) {
        struct efs_meta_chunk got; struct efs_meta_delta delta; uint32_t n; uint64_t seq;
        assert(read.ledger.state[i] == EFS_PACK_LIVE);
        assert(!efs_meta_apply_get_chunk_row(kv, records[i].ino, records[i].inode_generation, 0, &got, &delta, 1, &n, &seq));
        const uint8_t *bytes; uint32_t size;
        assert(!efs_pack_read(image, geometry, &got.packed, &bytes, &size));
        assert(size == sizeof(payload) && !memcmp(bytes, payload, size));
    }
    changes[0].before = NULL; changes[0].after = &chunks[0];
    assert(efs_pack_mapping_plan(kv, changes, 1, &plan) == EFS_ERR_STALE);
    /* Exercise the real publication apply, rather than only composing a
     * synthetic batch: delta preservation, then packed-to-ordinary promotion. */
    struct efs_meta_pub update = {.ino = records[1].ino,
        .inode_gen = records[1].inode_generation, .new_size = sizeof(payload),
        .now = 101, .expected_gen = chunks[1].generation, .candidate_gen = 1001,
        .coding_profile_id = EFS_META_PROFILE_K2F1, .delta_off = 33, .delta_len = 9};
    for (unsigned j = 0; j < EFS_NUM_FRAGMENTS; j++) {
        update.ch.nodes[j] = j + 1; memset(update.ch.checksums[j], 0x51 + j, EFS_HASH_SIZE);
    }
    assert(!efs_meta_apply_publish(kv, &update));
    struct efs_meta_chunk got; struct efs_meta_delta delta; uint32_t nd; uint64_t seq;
    assert(!efs_meta_apply_get_chunk_row(kv, records[1].ino, records[1].inode_generation,
        0, &got, &delta, 1, &nd, &seq));
    assert(got.coding_profile_id == EFS_META_PROFILE_PACK_K2F1 && nd == 1 && delta.len == 9);
    assert(!efs_pack_object_get(kv, group, 41, 43, &read));
    assert(read.ledger.state[1] == EFS_PACK_LIVE);
    update.delta_len = update.delta_off = 0; update.candidate_gen = 1003;
    update.delta_base_n = nd; update.delta_base_seq = seq;
    update.durable_result = update.fresh_object = 1;
    update.publication_id = owner; update.publication_id.seq = 23;
    update.receipt_locator = (struct efs_publication_locator){.group = group, .index = 77, .term = 1};
    if (fail_batch) {
        assert(!efs_kv_mem_fail_next_batch(kv));
        assert(efs_meta_apply_publish(kv, &update) != EFS_OK);
        assert(!efs_pack_object_get(kv, group, 41, 43, &read));
        assert(read.ledger.state[1] == EFS_PACK_LIVE);
        int verdict;
        assert(efs_meta_publication_result(kv, &update, &verdict) == EFS_ERR_NOT_FOUND);
        assert(!efs_meta_apply_get_chunk_row(kv, records[1].ino, records[1].inode_generation,
            0, &got, &delta, 1, &nd, &seq));
        assert(got.coding_profile_id == EFS_META_PROFILE_PACK_K2F1 && delta.len == 9);
    }
    assert(!efs_meta_apply_publish(kv, &update));
    int verdict; struct efs_publication_locator locator;
    assert(!efs_meta_publication_receipt(kv, &update, &verdict, &locator));
    assert(verdict == EFS_OK && locator.index == 77 && locator.term == 1);
    assert(!efs_meta_apply_publish(kv, &update)); /* Lost ACK: receipt, no resurrection. */
    assert(!efs_pack_object_get(kv, group, 41, 43, &read));
    assert(read.ledger.state[1] == EFS_PACK_RELEASED && read.ledger.state[2] == EFS_PACK_LIVE);
    assert(!efs_meta_apply_get_chunk_row(kv, records[1].ino, records[1].inode_generation,
        0, &got, &delta, 1, &nd, &seq));
    assert(got.coding_profile_id == EFS_META_PROFILE_K2F1 && got.generation == 1003);
    for (unsigned i = 2; i < N; i++) {
        changes[i].before = &chunks[i]; changes[i].after = NULL;
        batch[i - 2] = (struct efs_kv_item){EFS_KV_DEL, map_keys[i], kl[i], NULL, 0};
    }
    assert(!efs_pack_mapping_plan(kv, changes + 2, N - 2, &plan)); assert(plan.count == 1);
    batch[N - 2] = plan.items[0]; assert(!efs_kv_batch(kv, batch, N - 1));
    efs_pack_mapping_plan_free(&plan);
    assert(!efs_pack_object_get(kv, group, 41, 43, &read));
    assert(read.ledger.phase == EFS_PACK_RECLAIMING && !read.ledger.fence_acks && !read.ledger.delete_acks);
    for (unsigned i = 0; i < N; i++) assert(read.ledger.state[i] == EFS_PACK_RELEASED);
}
int main(void)
{
    struct efs_kv *kv = efs_kv_mem_create(); assert(kv); cohort(kv, 1); efs_kv_mem_free(kv);
    char dir[] = "/tmp/efs-pack-mapping-XXXXXX"; assert(mkdtemp(dir));
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        kv = efs_kv_lsm_open(dir, NULL); assert(kv);
        assert(!efs_kv_lsm_require_reader(kv, EFS_KV_READER_PACK_V1));
        cohort(kv, 0); _exit(0); /* Durable batches, no clean close or flush. */
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
    kv = efs_kv_lsm_open(dir, NULL); assert(kv);
    struct efs_pack_object recovered;
    assert(!efs_pack_object_get(kv, EFS_RAFT_GROUP_SHARD, 41, 43, &recovered));
    assert(recovered.ledger.phase == EFS_PACK_RECLAIMING);
    assert(!efs_kv_lsm_flush(kv)); assert(!efs_kv_lsm_compact(kv)); efs_kv_lsm_close(kv);
    kv = efs_kv_lsm_open(dir, NULL); assert(kv);
    struct efs_pack_object object; assert(!efs_pack_object_get(kv, EFS_RAFT_GROUP_SHARD, 41, 43, &object));
    assert(object.ledger.phase == EFS_PACK_RECLAIMING);
    for (unsigned i = 0; i < N; i++) assert(object.ledger.state[i] == EFS_PACK_RELEASED);
    efs_kv_lsm_close(kv); assert(!nftw(dir, cleanup, 32, FTW_DEPTH | FTW_PHYS));
    puts("pack mappings: atomic cohort/receipt, failed-batch isolation, EXCL guards, neighbor retention, no resurrection, abrupt WAL recovery and cold reclamation PASS");
}
