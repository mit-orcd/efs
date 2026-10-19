/* W125 packer metadata over a mem KV: admit a segment, switch staged files
 * into it with the exact-source CAS, and keep the ledger's live/dead counts
 * consistent with every later overwrite, unlink sweep and cancel. */
#include "efs/pack_mapping.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define T0 1000000000000000000ull
static const struct efs_meta_attrs at = {1000, 1000, T0};

static int count_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{ (void)k; (void)kl; (void)v; (void)vl; ++*(int *)u; return 0; }
static int gc_records(struct efs_kv *kv)
{
    int n = 0;
    for (uint32_t s = 0; s <= EFS_KV_SHARD_MASK; s++) {
        uint8_t p[3] = {(uint8_t)(s >> 8), (uint8_t)s, EFS_KV_KIND_GC};
        assert(efs_kv_scan_prefix(kv, p, 3, count_cb, &n) >= 0);
    }
    return n;
}
static void publish(struct efs_kv *kv, efs_ino_t ino, uint64_t expected, uint64_t gen, uint64_t size)
{
    struct efs_meta_pub p;
    memset(&p, 0, sizeof(p));
    p.ino = ino; p.new_size = size; p.expected_gen = expected; p.candidate_gen = gen;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        p.ch.nodes[i] = (efs_node_id_t)(i + 1);
        p.ch.checksums[i][0] = (uint8_t)gen; p.ch.checksums[i][1] = (uint8_t)i;
    }
    assert(efs_meta_apply_publish(kv, &p) == EFS_OK);
}
/* Files whose lane-0 shard is in group 0 (the segment's group). */
static efs_ino_t create(struct efs_kv *kv, const char *name)
{
    efs_ino_t ino = 0;
    assert(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, name, &ino) == EFS_OK);
    return ino;
}

int main(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    assert(kv && efs_meta_apply_init(kv, T0) == EFS_OK);
    enum { N = 6 };
    efs_ino_t ino[N]; uint64_t gen[N]; int made = 0;
    char name[32];
    for (int i = 0; made < N && i < 4096; i++) {
        snprintf(name, sizeof(name), "f%d", i);
        efs_ino_t x = create(kv, name);
        if (efs_raft_shard_group(efs_kv_lane_shard(x, 0)) != EFS_RAFT_GROUP_SHARD) continue;
        struct efs_meta_row r;
        assert(efs_meta_apply_get_inode(kv, x, &r) == EFS_OK);
        ino[made] = x; gen[made] = r.generation;
        publish(kv, x, 0, 0x100 + (uint64_t)made, 200 + (uint64_t)made);
        made++;
    }
    assert(made == N);
    int gc0 = gc_records(kv);

    /* Each small-file publication recorded a candidate in its group's index
     * (epoch from the leader stamp); the packer's view validates it; UNINDEX
     * drops it. */
    {
        int cand = 0;
        uint8_t p[3] = {0, 1, EFS_KV_KIND_PACK_CANDIDATE};
        assert(efs_kv_scan_prefix(kv, p, 3, count_cb, &cand) >= 0 && cand == N);
        struct efs_pack_candidate *c = calloc(1, sizeof(*c));
        assert(c && efs_meta_pack_candidate(kv, ino[0], gen[0], c) == EFS_OK);
        assert(c->size == 200 && c->nlink == 1 && S_ISREG(c->mode) && c->ndelta == 0 &&
               c->base.generation == 0x100 && c->parent == EFS_ROOT_INO);
        assert(efs_meta_pack_candidate(kv, ino[0], gen[0] + 1, c) == EFS_ERR_STALE);
        free(c);
        uint64_t ep = 0; /* publish() leaves the leader stamp at 0: epoch 0 */
        assert(efs_meta_apply_pack_unindex(kv, EFS_RAFT_GROUP_SHARD, &ep, &ino[N - 1], 1) == EFS_OK);
        cand = 0;
        assert(efs_kv_scan_prefix(kv, p, 3, count_cb, &cand) >= 0 && cand == N - 1);
        /* A file above the threshold is no candidate. */
        efs_ino_t big = create(kv, "big");
        if (efs_raft_shard_group(efs_kv_lane_shard(big, 0)) == EFS_RAFT_GROUP_SHARD) {
            publish(kv, big, 0, 0x777, EFS_PACK_THRESHOLD + 1);
            cand = 0;
            assert(efs_kv_scan_prefix(kv, p, 3, count_cb, &cand) >= 0 && cand == N - 1);
        }
    }

    /* Build one segment holding all six files' bytes. */
    uint8_t data[N][256];
    struct efs_pack_record rec[N];
    uint8_t digest[N][EFS_HASH_SIZE];
    for (int i = 0; i < N; i++) {
        struct efs_meta_chunk base; uint64_t epoch = 0;
        assert(efs_meta_pack_source(kv, ino[i], gen[i], digest[i], &base, &epoch) == EFS_OK);
        assert(base.generation == 0x100 + (uint64_t)i && base.coding_profile_id == EFS_META_PROFILE_K2F1);
        memset(data[i], 'a' + i, sizeof(data[i]));
        rec[i] = (struct efs_pack_record){.ino = ino[i], .inode_generation = gen[i],
            .content_epoch = epoch, .record_generation = 0x9000 + (uint64_t)i,
            .len = 200 + (uint32_t)i, .data = data[i]};
    }
    uint8_t *image = malloc(EFS_SEGMENT_MAX_SIZE); size_t geometry;
    struct efs_pack_ref refs[N];
    assert(image && !efs_pack_build(77, 1, EFS_RAFT_GROUP_SHARD, rec, N, image,
                                    EFS_SEGMENT_MAX_SIZE, &geometry, refs));
    struct efs_pack_object *o = calloc(1, sizeof(*o));
    struct efs_opid owner = {.client_uuid = {0xAC}, .session_epoch = 1, .seq = 1};
    assert(o && !efs_pack_ledger_init(image, geometry, refs, N, &owner, 0x7, 0, &o->ledger));
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) { o->nodes[i] = (efs_node_id_t)(i + 1); o->checksums[i][0] = 0xEE; }

    /* Admission: idempotent, refuses a different object under the same id,
     * and refuses the wrong group. */
    assert(efs_meta_apply_pack_admit(kv, EFS_RAFT_GROUP_SHARD2, o) == EFS_ERR_INVAL);
    assert(efs_meta_apply_pack_admit(kv, EFS_RAFT_GROUP_SHARD, o) == EFS_OK);
    assert(efs_meta_apply_pack_admit(kv, EFS_RAFT_GROUP_SHARD, o) == EFS_OK);
    {
        struct efs_pack_object *other = malloc(sizeof(*other));
        *other = *o; other->ledger.body_digest[0] ^= 1;
        assert(efs_meta_apply_pack_admit(kv, EFS_RAFT_GROUP_SHARD, other) == EFS_ERR_EXIST);
        free(other);
    }

    /* File 4 changes after the copy (its digest is stale) and is cancelled;
     * file 5 is left out of the switch and stays PENDING. */
    publish(kv, ino[4], 0x104, 0x204, 300);
    struct efs_pack_switch *sw = calloc(1, sizeof(*sw));
    sw->group = EFS_RAFT_GROUP_SHARD; sw->segment_id = 77; sw->segment_generation = 1;
    sw->count = N - 1; /* file 5 is left out and later cancelled */
    for (int i = 0; i < N - 1; i++)
        sw->members[i] = (struct efs_pack_switch_member){ino[i], gen[i], {0}, refs[i]};
    for (int i = 0; i < N - 1; i++) memcpy(sw->members[i].source_digest, digest[i], EFS_HASH_SIZE);

    /* The wire form round-trips exactly. */
    uint8_t *wire = malloc(19 + EFS_PACK_SWITCH_MAX * EFS_MD_PACK_SWITCH_MEMBER_BYTES); size_t wl;
    struct efs_pack_switch *back = calloc(1, sizeof(*back));
    assert(!efs_pack_switch_encode(sw, wire, 19 + EFS_PACK_SWITCH_MAX * EFS_MD_PACK_SWITCH_MEMBER_BYTES, &wl));
    assert(!efs_pack_switch_decode(wire, wl, back) && back->count == sw->count);
    assert(!memcmp(back->members[2].source_digest, digest[2], EFS_HASH_SIZE));
    assert(efs_pack_switch_decode(wire, wl - 1, back) == EFS_ERR_PROTO);

    uint32_t live = 0;
    assert(efs_meta_apply_pack_switch(kv, sw, &live) == EFS_OK && live == 4);
    struct efs_pack_object *cur = calloc(1, sizeof(*cur));
    assert(!efs_pack_object_get(kv, EFS_RAFT_GROUP_SHARD, 77, 1, cur));
    struct efs_pack_ledger_usage u;
    efs_pack_ledger_usage(&cur->ledger, &u);
    assert(u.live == 4 && u.dead == 1 && u.pending == 1);
    assert(cur->ledger.state[4] == EFS_PACK_CANCELLED);
    /* Switched rows are packed, keep the content epoch and name the segment;
     * each superseded per-file generation became one GC record. */
    for (int i = 0; i < 4; i++) {
        struct efs_meta_chunk ch;
        assert(efs_meta_apply_get_chunk(kv, ino[i], 0, &ch) == EFS_OK);
        assert(ch.coding_profile_id == EFS_META_PROFILE_PACK && ch.generation == 0x9000 + (uint64_t)i);
        assert(ch.packed.segment_id == 77 && ch.packed.len == 200 + (uint32_t)i);
        assert(ch.nodes[0] == 1 && ch.checksums[0][0] == 0xEE);
    }
    assert(gc_records(kv) == gc0 + 4);
    struct efs_meta_chunk still;
    assert(efs_meta_apply_get_chunk(kv, ino[4], 0, &still) == EFS_OK &&
           still.coding_profile_id == EFS_META_PROFILE_K2F1 && still.generation == 0x204);

    /* A replayed switch changes nothing: members are no longer PENDING. */
    assert(efs_meta_apply_pack_switch(kv, sw, &live) == EFS_OK && live == 0);
    assert(gc_records(kv) == gc0 + 4);

    /* Overwriting a packed file releases its record in the same batch and
     * queues no per-inode GC for the shared segment. */
    publish(kv, ino[0], 0x9000, 0x300, 100);
    assert(!efs_pack_object_get(kv, EFS_RAFT_GROUP_SHARD, 77, 1, cur));
    efs_pack_ledger_usage(&cur->ledger, &u);
    assert(u.live == 3 && u.dead == 2 && u.live_bytes == 201 + 202 + 203);
    assert(gc_records(kv) == gc0 + 4);

    /* Cancelling the abandoned record (file 5) leaves only live and dead. */
    assert(efs_meta_apply_pack_cancel(kv, EFS_RAFT_GROUP_SHARD, 77, 1) == EFS_OK);
    assert(!efs_pack_object_get(kv, EFS_RAFT_GROUP_SHARD, 77, 1, cur));
    efs_pack_ledger_usage(&cur->ledger, &u);
    assert(u.live == 3 && u.dead == 3 && !u.pending && cur->ledger.phase == EFS_PACK_ACTIVE);

    /* Lane sweep of packed rows (the unlink/reap path) releases each record;
     * when the last one goes, the segment starts reclaiming. */
    for (int i = 1; i < 4; i++)
        assert(efs_meta_apply_lane_sweep(kv, ino[i], gen[i], 0) == EFS_OK);
    assert(!efs_pack_object_get(kv, EFS_RAFT_GROUP_SHARD, 77, 1, cur));
    efs_pack_ledger_usage(&cur->ledger, &u);
    assert(!u.live && u.dead == N && cur->ledger.phase == EFS_PACK_RECLAIMING);
    assert(gc_records(kv) == gc0 + 4);
    /* A switch into a reclaiming segment is refused outright. */
    assert(efs_meta_apply_pack_switch(kv, sw, &live) == EFS_ERR_STALE);

    /* RETIRE forgets a reclaiming segment, skips an ACTIVE one, and a
     * replay (or an unknown id) is a no-op. */
    {
        struct efs_pack_object *act = calloc(1, sizeof(*act));
        assert(act && !efs_pack_build(78, 1, EFS_RAFT_GROUP_SHARD, rec, 1, image,
                                      EFS_SEGMENT_MAX_SIZE, &geometry, refs));
        assert(!efs_pack_ledger_init(image, geometry, refs, 1, &owner, 0x7, 0, &act->ledger));
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) act->nodes[i] = (efs_node_id_t)(i + 1);
        assert(efs_meta_apply_pack_admit(kv, EFS_RAFT_GROUP_SHARD, act) == EFS_OK);
        uint64_t ids[3] = {77, 78, 999}, gens[3] = {1, 1, 1};
        uint32_t retired = 9;
        assert(efs_meta_apply_pack_retire(kv, EFS_RAFT_GROUP_SHARD, ids, gens, 3, &retired) == EFS_OK);
        assert(retired == 1);
        assert(efs_pack_object_get(kv, EFS_RAFT_GROUP_SHARD, 77, 1, cur) == EFS_ERR_NOT_FOUND);
        assert(efs_pack_object_get(kv, EFS_RAFT_GROUP_SHARD, 78, 1, cur) == EFS_OK);
        assert(efs_meta_apply_pack_retire(kv, EFS_RAFT_GROUP_SHARD, ids, gens, 3, &retired) == EFS_OK &&
               retired == 0);
        free(act);
    }

    free(image); free(o); free(sw); free(wire); free(back); free(cur);
    efs_kv_mem_free(kv);
    puts("pack switch: admit, exact-source CAS, cancel, replay, overwrite release, sweep release, retire, candidate index PASS");
    return 0;
}
