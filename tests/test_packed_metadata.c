#define _XOPEN_SOURCE 700
#include "efs/meta_apply.h"
#include "efs/metadata.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/raft.h"
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Independently construct persisted bytes: the production reader must locate
 * deltas after the packed reference, bind FileID, and reject damaged trailers. */
enum { BASE = 128, PACK = BASE + EFS_PACK_REF_BYTES, DELTA = 140 };
static void put(uint8_t *p, uint64_t x, unsigned n)
{ while (n) { p[--n] = (uint8_t)x; x >>= 8; } }
static int cleanup(const char *p, const struct stat *st, int flag, struct FTW *f)
{ (void)st; (void)flag; (void)f; return remove(p); }
static void rejected(struct efs_kv *kv, const uint8_t *key, uint32_t kl,
    uint8_t *value, uint32_t vl, efs_ino_t ino, uint64_t gen)
{
    struct efs_meta_chunk ch; struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
    uint32_t n; uint64_t seq;
    assert(!efs_kv_put(kv, key, kl, value, vl));
    assert(efs_meta_apply_get_chunk_row(kv, ino, gen, 0, &ch, ds,
        EFS_CHUNK_DELTA_MAX, &n, &seq) == EFS_ERR_PROTO);
    assert(efs_meta_apply_get_chunk(kv, ino, 0, &ch) == EFS_ERR_PROTO);
    assert(efs_meta_apply_get_chunk_deltas(kv, ino, 0, ds,
        EFS_CHUNK_DELTA_MAX, &n, &seq) == EFS_ERR_PROTO);
    assert(efs_meta_apply_chunk_holds(kv, ino, gen, 0, 19, 0, 0, NULL) == EFS_ERR_PROTO);
}
static void exercise(struct efs_kv *kv, efs_ino_t ino, uint64_t gen)
{
    uint8_t key[EFS_KV_KEY_MAX], other[EFS_KV_KEY_MAX]; uint32_t kl, ol;
    uint8_t value[PACK + 4 + DELTA], saved[sizeof(value)];
    uint8_t payload[512], image[EFS_SEGMENT_MAX_SIZE]; size_t geometry;
    memset(payload, 0xa7, sizeof(payload));
    struct efs_pack_record record = {ino, gen, 3, 19, sizeof(payload), payload};
    struct efs_pack_ref ref;
    uint32_t group = efs_raft_shard_group(efs_kv_lane_shard(ino, 0));
    assert(!efs_pack_build(41, 43, group, &record, 1, image, sizeof(image), &geometry, &ref));
    assert(!efs_kv_key_chunk(efs_kv_lane_shard(ino, 0), ino, gen, 0, 0, key, &kl));
    memset(value, 0, sizeof(value));
    put(value, 19, 8); put(value + 8, EFS_META_PROFILE_PACK_K2F1, 4);
    put(value + 12, 3, 8);
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) put(value + 20 + i * 4, i + 1, 4);
    memset(value + 32, 0xc3, 96);
    assert(!efs_pack_ref_encode(&ref, value + BASE));
    struct efs_meta_chunk ch; struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
    uint32_t n; uint64_t seq;
    assert(!efs_kv_put(kv, key, kl, value, PACK));
    assert(!efs_meta_apply_get_chunk_row(kv, ino, gen, 0, &ch, ds,
        EFS_CHUNK_DELTA_MAX, &n, &seq));
    assert(!n && !seq && ch.packed.segment_id == 41 && ch.packed.len == 512);
    const uint8_t *bytes; uint32_t len;
    assert(!efs_pack_read(image, geometry, &ch.packed, &bytes, &len));
    assert(len == sizeof(payload) && !memcmp(bytes, payload, len));
    /* Ordinary deltas retain their format but follow the extended base. */
    put(value + PACK, 1, 2);
    uint8_t *d = value + PACK + 4;
    put(d, 17, 4); put(d + 4, 29, 4); put(d + 8, 23, 8);
    put(d + 16, 3, 8); put(d + 24, 7, 8);
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) put(d + 32 + i * 4, i + 1, 4);
    memset(d + 44, 0xd4, 96);
    memcpy(saved, value, sizeof(value));
    assert(!efs_kv_put(kv, key, kl, value, sizeof(value)));
    assert(!efs_meta_apply_get_chunk_row(kv, ino, gen, 0, &ch, ds,
        EFS_CHUNK_DELTA_MAX, &n, &seq));
    assert(n == 1 && seq == 7 && ds[0].off == 17 && ds[0].len == 29 && ds[0].generation == 23);
    assert(efs_meta_apply_chunk_holds(kv, ino, gen, 0, 23, 17, 29, NULL) == 1);
    assert(!efs_meta_apply_get_chunk_deltas(kv, ino, 0, ds, EFS_CHUNK_DELTA_MAX, &n, &seq));
    assert(n == 1 && seq == 7);
    /* Truncation, garbage, noncanonical count/padding and identity changes. */
    const uint32_t bad_lengths[] = {BASE, PACK - 1, PACK + 1, sizeof(value) - 1};
    for (unsigned i = 0; i < sizeof(bad_lengths)/sizeof(bad_lengths[0]); i++)
        rejected(kv, key, kl, value, bad_lengths[i], ino, gen);
    const unsigned bad_offsets[] = {7, 19, BASE + 7, BASE + 15, BASE + 55, PACK + 1, PACK + 2};
    for (unsigned i = 0; i < sizeof(bad_offsets)/sizeof(bad_offsets[0]); i++) {
        memcpy(value, saved, sizeof(value)); value[bad_offsets[i]] ^= 1;
        rejected(kv, key, kl, value, sizeof(value), ino, gen);
    }
    memcpy(value, saved, sizeof(value));
    assert(!efs_kv_put(kv, key, kl, value, sizeof(value)));
    assert(!efs_kv_key_chunk(efs_kv_lane_shard(ino, 0), ino, gen + 1, 0, 0, other, &ol));
    assert(!efs_kv_put(kv, other, ol, value, sizeof(value)));
    assert(efs_meta_apply_get_chunk_row(kv, ino, gen + 1, 0, &ch, ds,
        EFS_CHUNK_DELTA_MAX, &n, &seq) == EFS_ERR_PROTO);
    assert(!efs_kv_del(kv, other, ol));
    assert(!efs_kv_key_chunk(efs_kv_lane_shard(ino, 1), ino, gen, 1, 1, other, &ol));
    assert(!efs_kv_put(kv, other, ol, value, sizeof(value)));
    assert(efs_meta_apply_get_chunk_row(kv, ino, gen, 1, &ch, ds,
        EFS_CHUNK_DELTA_MAX, &n, &seq) == EFS_ERR_PROTO);
    assert(!efs_kv_del(kv, other, ol));
}
int main(void)
{
    char dir[] = "/tmp/efs-packed-metadata-XXXXXX";
    assert(mkdtemp(dir));
    struct efs_kv *kv = efs_kv_lsm_open(dir, NULL); assert(kv);
    assert(!efs_kv_lsm_require_reader(kv, EFS_KV_READER_PACK_V1));
    assert(!efs_meta_apply_init(kv, 100));
    const struct efs_meta_attrs at = {1000, 1000, 100};
    efs_ino_t ino; struct efs_meta_row row;
    assert(!efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0600, "packed", &ino));
    assert(!efs_meta_apply_get_inode(kv, ino, &row));
    exercise(kv, ino, row.generation);
    assert(!efs_kv_lsm_flush(kv)); assert(!efs_kv_lsm_compact(kv));
    efs_kv_lsm_close(kv);
    kv = efs_kv_lsm_open(dir, NULL); assert(kv);
    struct efs_meta_chunk ch; struct efs_meta_delta d; uint32_t n; uint64_t seq;
    assert(!efs_meta_apply_get_chunk_row(kv, ino, row.generation, 0, &ch, &d, 1, &n, &seq));
    assert(ch.packed.segment_id == 41 && ch.packed.len == 512 && n == 1 && seq == 7);
    efs_kv_lsm_close(kv);
    assert(!nftw(dir, cleanup, 32, FTW_DEPTH | FTW_PHYS));
    puts("packed metadata: cold LSM, FileID binding, mixed delta offsets and corrupt-image refusal PASS");
    return 0;
}
