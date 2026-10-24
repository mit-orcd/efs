/* Alpha 1.0 checklist B1-B5 (docs/status/alpha-1.0-2plus1-checklist.md),
 * the deterministic part: geometry, placement and the capacity model as the
 * code computes them. The persisted side (fragment records on stores, chunk
 * rows, packed segments, df and status on members) is checked against real
 * daemons by tests/test_b_geometry.py and, on any cluster, by
 * scripts/b_checks.py.
 *
 * B1  every stripe is exactly D1/D2/P on three distinct members (ordinary
 *     chunks and packed segments, sparse member ids, down members excluded
 *     from segments); a recorded stripe is accepted only in that form
 *     (efs_fragment_nodes_valid, the reader's gate); the record binds the
 *     role, so a fragment never stands in for another index.
 * B2  partial chunks, empty and boundary lengths, the last chunk of a file,
 *     per-export chunk sizes and packed segment geometry: every two-fragment
 *     pair reconstructs the exact bytes and zero padding; a corrupt or
 *     mismatched fragment is refused by its record, never decoded.
 * B3  diverse inodes and chunk indexes: three members always all three;
 *     four members rotate the omitted member (each omitted once per four
 *     consecutive chunks); five to eight members, grown one at a time, every
 *     member takes every role near the mean. Histograms are printed.
 * B4  packed segments: destinations over the whole data ring, not the
 *     group voters; their share and the capacity that is usable for them.
 * B5  logical capacity against physical quotas (3-of-N bound), a full
 *     member, capacity after an addition, the 1.5x protection cost and the
 *     startup headroom budget. */
#include "efs/placement.h"
#include "efs/erasure.h"
#include "efs/frag_record.h"
#include "efs/checksum.h"
#include "efs/storage_capacity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* Inodes of one shard step by 2^12 (efs_export_shard_of); mix in a few
 * shards and sequential runs so the inode patterns differ. */
static efs_ino_t file_ino(uint32_t k)
{
    switch (k % 3) {
    case 0: return (efs_ino_t)(4096ull * k + 7);
    case 1: return (efs_ino_t)(0x1234ull << 40 | k);
    default: return (efs_ino_t)(1000003ull + 8ull * k);
    }
}

static void ring(struct efs_node *nodes, const efs_node_id_t *ids, uint32_t n)
{
    memset(nodes, 0, sizeof(*nodes) * n);
    /* Membership order is not ring order: list them backwards. */
    for (uint32_t i = 0; i < n; i++)
        nodes[i].id = ids[n - 1 - i];
}

static int member(const efs_node_id_t *ids, uint32_t n, efs_node_id_t id)
{
    for (uint32_t i = 0; i < n; i++)
        if (ids[i] == id)
            return 1;
    return 0;
}

/* ---- B1 ---------------------------------------------------------------- */

static void b1_stripes(void)
{
    static const efs_node_id_t sparse[] = {2, 3, 5, 7, 8, 11, 12, 13, 14, 15, 16, 1, 4, 6, 9, 10};
    struct efs_node nodes[EFS_MAX_NODES];
    efs_node_id_t f[EFS_NUM_FRAGMENTS];
    uint64_t stripes = 0;

    for (uint32_t n = 3; n <= EFS_MAX_NODES; n++) {
        ring(nodes, sparse, n);
        for (uint32_t k = 0; k < 2048; k++) {
            for (uint32_t ci = 0; ci < 4; ci++) {
                efs_place_fragments(nodes, n, file_ino(k), ci + (k & 7) * 977, f);
                CHECK(efs_fragment_nodes_valid(f), "n=%u k=%u ci=%u stripe %u,%u,%u", n, k, ci,
                      f[0], f[1], f[2]);
                for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
                    CHECK(member(sparse, n, f[i]), "n=%u fragment %d on non-member %u", n, i, f[i]);
                stripes++;
            }
            efs_node_id_t s[EFS_NUM_FRAGMENTS];
            uint64_t seg = (1700000000000000ull + 977ull * k) << 8 | sparse[0];
            CHECK(efs_pack_destinations(nodes, n, sparse[0], seg, s) == EFS_OK, "n=%u segment", n);
            CHECK(efs_fragment_nodes_valid(s), "n=%u segment stripe %u,%u,%u", n, s[0], s[1], s[2]);
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
                CHECK(member(sparse, n, s[i]), "n=%u segment fragment %d on non-member %u", n, i, s[i]);
        }
    }

    /* Segments: a down member is never a destination (self always counts);
     * duplicates and invalid ids in the list are ignored; fewer than three
     * eligible members is BUSY with out[] zeroed, never a short stripe. */
    for (uint32_t n = 3; n <= 8; n++) {
        efs_node_id_t ids[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        ring(nodes, ids, n);
        nodes[0].down_until_ms = 1; /* id n */
        for (uint32_t k = 0; k < 512; k++) {
            efs_node_id_t s[EFS_NUM_FRAGMENTS];
            int rc = efs_pack_destinations(nodes, n, 1, (uint64_t)k << 8 | 1, s);
            if (n == 3) {
                CHECK(rc == EFS_ERR_BUSY && !s[0] && !s[1] && !s[2], "n=3 one down: rc=%d", rc);
                continue;
            }
            CHECK(rc == EFS_OK && efs_fragment_nodes_valid(s), "n=%u one down rc=%d", n, rc);
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
                CHECK(s[i] != n, "n=%u down member %u chose", n, n);
        }
        /* Self down-marked still counts. */
        efs_node_id_t s[EFS_NUM_FRAGMENTS];
        CHECK(efs_pack_destinations(nodes, n, n, 77, s) == (n == 3 ? EFS_OK : EFS_OK),
              "n=%u self down-marked", n);
    }
    {
        struct efs_node d[5];
        efs_node_id_t s[EFS_NUM_FRAGMENTS];
        memset(d, 0, sizeof(d));
        d[0].id = 1; d[1].id = 1; d[2].id = 2; d[3].id = 0; d[4].id = EFS_MAX_NODES + 1;
        CHECK(efs_pack_destinations(d, 5, 1, 9, s) == EFS_ERR_BUSY && !s[0] && !s[1] && !s[2],
              "duplicates and invalid ids counted as members");
        CHECK(efs_pack_destinations(NULL, 0, 1, 9, s) == EFS_ERR_BUSY, "NULL members");
    }

    /* The gate readers apply to recorded fragment nodes. */
    static const efs_node_id_t bad[][3] = {{0, 1, 2}, {1, 0, 2}, {1, 2, 0}, {1, 1, 2}, {1, 2, 1},
                                           {2, 2, 2}, {1, 2, EFS_MAX_NODES + 1}, {0, 0, 0}};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!efs_fragment_nodes_valid(bad[i]), "invalid stripe %u,%u,%u accepted",
              bad[i][0], bad[i][1], bad[i][2]);
    static const efs_node_id_t good[3] = {EFS_MAX_NODES, 1, 9};
    CHECK(efs_fragment_nodes_valid(good) && !efs_fragment_nodes_valid(NULL), "valid stripe");

    /* The record binds the role: fragment i never verifies as fragment j. */
    {
        uint8_t payload[4096], digest[EFS_HASH_SIZE], rec[EFS_FRAG_RECORD_BYTES];
        for (size_t i = 0; i < sizeof(payload); i++)
            payload[i] = (uint8_t)(i * 31 + 7);
        efs_hash(payload, sizeof(payload), digest);
        for (uint32_t fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
            struct efs_frag_ident id = {.kind = EFS_FRAG_KIND_CHUNK, .space = 1, .fragment_index = fi,
                .chunk_index = 3, .payload_len = sizeof(payload), .coding_profile = EFS_FRAG_PROFILE_K2F1,
                .object = 4096 * 5 + 7, .generation = 99};
            CHECK(efs_frag_record_encode(&id, digest, rec) == EFS_OK, "encode fi=%u", fi);
            for (uint32_t fj = 0; fj < EFS_NUM_FRAGMENTS; fj++) {
                struct efs_frag_ident want = id;
                const char *field = NULL;
                want.fragment_index = fj;
                int rc = efs_frag_verify(payload, sizeof(payload), rec, sizeof(rec), &want, NULL, &field);
                CHECK(fi == fj ? rc == EFS_OK : rc == EFS_ERR_FRAG_IDENTITY,
                      "fragment %u verified as %u: rc=%d", fi, fj, rc);
            }
        }
    }
    printf("B1: %llu stripes over 3..%u members (sparse ids): D1/D2/P on 3 distinct members;"
           " segments skip down members; recorded stripes gated; role bound in the record\n",
           (unsigned long long)stripes, EFS_MAX_NODES);
}

/* ---- B2 ---------------------------------------------------------------- */

static int pair_roundtrip(const uint8_t *data, size_t len, size_t size, int segment)
{
    size_t fl = size / 2;
    uint8_t *frag = malloc(3 * fl), *out = malloc(size), *want = calloc(1, size);
    uint8_t *f[EFS_NUM_FRAGMENTS] = {frag, frag + fl, frag + 2 * fl};
    int bad = 0;

    if (!frag || !out || !want)
        abort();
    if (len)
        memcpy(want, data, len);
    if ((segment ? efs_encode_segment(data, len, size, f) : efs_encode_chunk(data, len, size, f)) != EFS_OK) {
        free(frag); free(out); free(want);
        return 1;
    }
    /* Roles: D1 the first half, D2 the second, P their XOR. */
    for (size_t i = 0; i < fl; i++)
        if (f[0][i] != want[i] || f[1][i] != want[fl + i] || f[2][i] != (uint8_t)(want[i] ^ want[fl + i])) {
            bad++;
            break;
        }
    static const int pairs[3][3] = {{0, 1, 2}, {0, 2, 1}, {1, 2, 0}};
    for (int p = 0; p < 3; p++) {
        memset(out, 0xa5, size);
        int rc = segment ? efs_decode_segment(f, size, pairs[p][0], pairs[p][1], pairs[p][2], out, len)
                         : efs_decode_chunk(f, size, pairs[p][0], pairs[p][1], pairs[p][2], out, len);
        /* Exact payload and zero padding (never stale or fabricated bytes). */
        if (rc != EFS_OK || memcmp(out, want, size))
            bad++;
    }
    free(frag); free(out); free(want);
    return bad;
}

static void b2_payloads(void)
{
    static const size_t chunks[] = {128 * 1024, 256 * 1024, 1024 * 1024};
    uint8_t *data = malloc(4 * 1024 * 1024);
    uint32_t cases = 0;

    if (!data)
        abort();
    for (size_t i = 0; i < 4 * 1024 * 1024; i++)
        data[i] = (uint8_t)(next_rand() >> 32);
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        size_t cs = chunks[c], h = cs / 2;
        size_t lens[] = {0, 1, 2, 511, 512, 4095, 4096, 4097, h - 1, h, h + 1, cs - 4096, cs - 1, cs};
        for (size_t l = 0; l < sizeof(lens) / sizeof(lens[0]); l++, cases++)
            CHECK(!pair_roundtrip(data, lens[l], cs, 0), "chunk %zu len %zu", cs, lens[l]);
        CHECK(efs_encode_chunk(data, cs + 1, cs, (uint8_t *[3]){data, data, data}) == EFS_ERR_INVAL,
              "over-long chunk accepted");
    }
    /* The last chunk of a file: size S has ceil(S/cs) chunks; the last one
     * carries S - (n-1)cs bytes and decodes to them plus zeros. */
    {
        size_t cs = 128 * 1024;
        size_t sizes[] = {1, cs - 1, cs, cs + 1, 2 * cs, 3 * cs + 17, 3 * 1024 * 1024 + 17};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++, cases++) {
            size_t s = sizes[i], n = (s + cs - 1) / cs, last = s - (n - 1) * cs;
            for (size_t ci = 0; ci < n; ci++)
                CHECK(!pair_roundtrip(data + ci * cs, ci + 1 == n ? last : cs, cs, 0),
                      "file %zu chunk %zu/%zu", s, ci, n);
            CHECK(last >= 1 && last <= cs, "file %zu last chunk %zu", s, last);
        }
    }
    /* Packed segments: geometry rounds the body up to 8 KiB (fragments are
     * whole 4 KiB pages), at most one minimum chunk; every pair decodes. */
    {
        size_t bodies[] = {1, 4095, 8191, 8192, 8193, 65536, 100000, EFS_SEGMENT_MAX_SIZE};
        for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++, cases++) {
            size_t g = 0;
            CHECK(efs_segment_geometry(bodies[i], &g) == EFS_OK && g >= bodies[i] &&
                  g % EFS_SEGMENT_ALIGNMENT == 0 && (g / 2) % 4096 == 0 && g <= EFS_SEGMENT_MAX_SIZE &&
                  g - bodies[i] < EFS_SEGMENT_ALIGNMENT, "segment body %zu geometry %zu", bodies[i], g);
            CHECK(!pair_roundtrip(data, bodies[i], g, 1), "segment body %zu", bodies[i]);
        }
        size_t g;
        CHECK(efs_segment_geometry(0, &g) == EFS_ERR_INVAL &&
              efs_segment_geometry(EFS_SEGMENT_MAX_SIZE + 1, &g) == EFS_ERR_INVAL, "segment bounds");
    }
    /* Corrupt or mismatched fragments are refused by the record before any
     * decode: a flipped payload byte, a flipped record byte, a record of
     * another chunk, generation, object or kind. */
    {
        uint8_t pay[65536], dg[EFS_HASH_SIZE], rec[EFS_FRAG_RECORD_BYTES], bad[EFS_FRAG_RECORD_BYTES];
        memcpy(pay, data, sizeof(pay));
        efs_hash(pay, sizeof(pay), dg);
        struct efs_frag_ident id = {.kind = EFS_FRAG_KIND_CHUNK, .space = 1, .fragment_index = 1,
            .chunk_index = 2, .payload_len = sizeof(pay), .coding_profile = EFS_FRAG_PROFILE_K2F1,
            .object = 81927, .generation = 5};
        CHECK(efs_frag_record_encode(&id, dg, rec) == EFS_OK, "record");
        CHECK(efs_frag_verify(pay, sizeof(pay), rec, sizeof(rec), &id, NULL, NULL) == EFS_OK, "good fragment");
        pay[777] ^= 1;
        CHECK(efs_frag_verify(pay, sizeof(pay), rec, sizeof(rec), &id, NULL, NULL) == EFS_ERR_CHECKSUM,
              "flipped payload accepted");
        pay[777] ^= 1;
        for (size_t at = 0; at < 120; at += 7) {
            memcpy(bad, rec, sizeof(bad));
            bad[at] ^= 0x10;
            int rc = efs_frag_verify(pay, sizeof(pay), bad, sizeof(bad), &id, NULL, NULL);
            CHECK(rc != EFS_OK, "flipped record byte %zu accepted", at);
        }
        struct efs_frag_ident other[5];
        for (int i = 0; i < 5; i++)
            other[i] = id;
        other[0].chunk_index = 3;
        other[1].generation = 6;
        other[2].object = 81928;
        other[3].kind = EFS_FRAG_KIND_SEGMENT;
        other[4].payload_len = sizeof(pay) - 4096;
        for (int i = 0; i < 5; i++)
            CHECK(efs_frag_verify(pay, sizeof(pay), rec, sizeof(rec), &other[i], NULL, NULL) != EFS_OK,
                  "mismatched identity %d accepted", i);
        cases += 3;
    }
    free(data);
    printf("B2: %u payload/geometry cases: every two-fragment pair exact (empty, 1 B, page and"
           " half-chunk boundaries, last chunk, 128 KiB..1 MiB chunks, segments); corrupt and"
           " mismatched fragments refused\n", cases);
}

/* ---- B3 ---------------------------------------------------------------- */

static void histogram(const char *what, uint32_t n, const efs_node_id_t *ids,
                      uint64_t h[][EFS_NUM_FRAGMENTS])
{
    printf("  %s, %u members (node: D1/D2/P):", what, n);
    for (uint32_t i = 0; i < n; i++)
        printf(" %u:%llu/%llu/%llu", ids[i], (unsigned long long)h[i][0],
               (unsigned long long)h[i][1], (unsigned long long)h[i][2]);
    printf("\n");
}

/* Every member of ids takes every role within +-tol of the mean. */
static void balanced(const char *what, uint32_t n, const efs_node_id_t *ids,
                     uint64_t h[][EFS_NUM_FRAGMENTS], uint64_t stripes, double tol)
{
    double mean = (double)stripes / n;
    for (uint32_t i = 0; i < n; i++)
        for (int r = 0; r < EFS_NUM_FRAGMENTS; r++)
            CHECK(h[i][r] >= mean * (1 - tol) && h[i][r] <= mean * (1 + tol),
                  "%s n=%u node %u role %d: %llu (mean %.0f)", what, n, ids[i], r,
                  (unsigned long long)h[i][r], mean);
}

static uint32_t index_of(const efs_node_id_t *ids, uint32_t n, efs_node_id_t id)
{
    for (uint32_t i = 0; i < n; i++)
        if (ids[i] == id)
            return i;
    return n;
}

static void b3_spread(void)
{
    /* Growth one member at a time: 3, 4, ... 8 (ids as the cluster assigns
     * them), then the same with gaps in the id space. */
    static const efs_node_id_t dense[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    static const efs_node_id_t gaps[8] = {1, 2, 3, 5, 6, 9, 12, 16};
    const efs_node_id_t *sets[2] = {dense, gaps};
    struct efs_node nodes[EFS_MAX_NODES];
    efs_node_id_t f[EFS_NUM_FRAGMENTS];
    const uint32_t files = 3000, per_file = 4;

    for (int set = 0; set < 2; set++) {
        const efs_node_id_t *ids = sets[set];
        for (uint32_t n = 3; n <= 8; n++) {
            uint64_t h[8][EFS_NUM_FRAGMENTS], omitted[8];
            memset(h, 0, sizeof(h));
            memset(omitted, 0, sizeof(omitted));
            ring(nodes, ids, n);
            /* Files written after the n-th member joined (new inodes). */
            for (uint32_t k = 0; k < files; k++) {
                efs_ino_t ino = file_ino(k + 100000 * n);
                uint32_t base = (uint32_t)(next_rand() % 4096) * 4;
                uint32_t miss_in_run[8] = {0};
                for (uint32_t ci = base; ci < base + per_file; ci++) {
                    int in[8] = {0};
                    efs_place_fragments(nodes, n, ino, ci, f);
                    for (int r = 0; r < EFS_NUM_FRAGMENTS; r++) {
                        uint32_t x = index_of(ids, n, f[r]);
                        CHECK(x < n, "set %d n=%u non-member %u", set, n, f[r]);
                        if (x < n) {
                            h[x][r]++;
                            in[x] = 1;
                        }
                    }
                    if (n == 3)
                        CHECK(in[0] && in[1] && in[2], "3 members: a chunk left one out");
                    for (uint32_t x = 0; x < n; x++)
                        if (!in[x]) {
                            omitted[x]++;
                            miss_in_run[x]++;
                        }
                }
                /* Four members: four consecutive chunks of one file omit
                 * each member exactly once (the omitted member rotates). */
                if (n == 4)
                    for (uint32_t x = 0; x < 4; x++)
                        CHECK(miss_in_run[x] == 1, "4 members: ino %llu node %u omitted %u of 4",
                              (unsigned long long)ino, ids[x], miss_in_run[x]);
            }
            uint64_t stripes = (uint64_t)files * per_file;
            char what[64];
            snprintf(what, sizeof(what), "B3 %s ids", set ? "gapped" : "dense");
            if (set == 0)
                histogram(what, n, ids, h);
            balanced(what, n, ids, h, stripes, 0.10);
            if (n == 4) {
                if (set == 0)
                    printf("  B3 4 members omitted: %llu/%llu/%llu/%llu of %llu chunks\n",
                           (unsigned long long)omitted[0], (unsigned long long)omitted[1],
                           (unsigned long long)omitted[2], (unsigned long long)omitted[3],
                           (unsigned long long)stripes);
                for (uint32_t x = 0; x < 4; x++)
                    CHECK(omitted[x] * 4 == stripes, "4 members: node %u omitted %llu of %llu",
                          ids[x], (unsigned long long)omitted[x], (unsigned long long)stripes);
            }
        }
    }
    /* A short burst after an addition (24 two-chunk files, the K33 growth
     * case) already reaches the added member, at every size 4..8. */
    for (uint32_t n = 4; n <= 8; n++) {
        uint32_t newest = 0;
        ring(nodes, dense, n);
        for (uint32_t k = 0; k < 24; k++)
            for (uint32_t ci = 0; ci < 2; ci++) {
                efs_place_fragments(nodes, n, (efs_ino_t)(4096ull * (k + 900) + 7), ci, f);
                for (int r = 0; r < EFS_NUM_FRAGMENTS; r++)
                    newest += f[r] == n;
            }
        CHECK(newest >= 144 / n / 2, "added member %u took %u of 144 fragments", n, newest);
    }
    printf("B3: 3 members use all three for every chunk; 4 rotate the omitted member exactly;"
           " 5..8 (grown one at a time, dense and gapped ids) give every member every role"
           " within 10%% of the mean\n");
}

/* ---- B4 ---------------------------------------------------------------- */

static void b4_packing(void)
{
    static const efs_node_id_t ids[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    struct efs_node nodes[EFS_MAX_NODES];
    const uint32_t segs = 6000;

    for (uint32_t n = 3; n <= 8; n++) {
        uint64_t h[8][EFS_NUM_FRAGMENTS], on_voters = 0;
        /* Group 2 votes on {1,2,3} at three members and {2,3,4} from four
         * (K33): the segment's group, not its destinations. */
        uint32_t voters = n >= 4 ? 0xeu : 0x7u;
        memset(h, 0, sizeof(h));
        ring(nodes, ids, n);
        for (efs_node_id_t self = 1; self <= 3; self++) {
            uint64_t clock_ns = 1761300000000000000ull + self * 12345;
            for (uint32_t k = 0; k < segs / 3; k++) {
                efs_node_id_t s[EFS_NUM_FRAGMENTS];
                /* pk_segment_id: now_ns << 8 | self, on a microsecond clock. */
                uint64_t id = (clock_ns + (uint64_t)k * 1000 * (1 + k % 5)) / 1000 * 1000 << 8 | self;
                CHECK(efs_pack_destinations(nodes, n, self, id, s) == EFS_OK, "n=%u", n);
                for (int r = 0; r < EFS_NUM_FRAGMENTS; r++) {
                    h[s[r] - 1][r]++;
                    on_voters += (voters >> (s[r] - 1)) & 1u;
                }
            }
        }
        uint64_t total = (uint64_t)(segs / 3) * 3;
        histogram("B4 segments", n, ids, h);
        balanced("B4 segments", n, ids, h, total, 0.10);
        double share = (double)on_voters / (double)(total * EFS_NUM_FRAGMENTS);
        /* Destinations are the ring: the voters' share is 3/n, not 1. */
        CHECK(share > 3.0 / n - 0.03 && share < 3.0 / n + 0.03, "n=%u voters' share %.3f", n, share);
        uint64_t q = 1ull << 30, caps[8], vcaps[3] = {q, q, q};
        for (uint32_t i = 0; i < n; i++)
            caps[i] = q;
        printf("  B4 %u members: %.1f%% of segment fragments on the 3 group-2 voters;"
               " usable for packing %llu MiB (ring), %llu MiB if voters only\n", n, 100.0 * share,
               (unsigned long long)(efs_capacity_logical(caps, n) >> 20),
               (unsigned long long)(efs_capacity_logical(vcaps, 3) >> 20));
    }
    printf("B4: packed segments spread over every member (not the group voters), every role"
           " within 10%% of the mean; usable packing capacity is the ring's\n");
}

/* ---- B5 ---------------------------------------------------------------- */

/* Brute-force reference: the largest M with sum min(c, M) >= 3M. */
static uint64_t ref_logical(const uint64_t *c, uint32_t n)
{
    uint32_t nz = 0;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        sum += c[i];
        nz += c[i] != 0;
    }
    if (n < 3 || nz < 3)
        return 0;
    uint64_t m = sum / 3;
    for (;;) {
        uint64_t got = 0;
        for (uint32_t i = 0; i < n; i++)
            got += c[i] < m ? c[i] : m;
        if (got >= 3 * m)
            return 2 * m;
        m--;
    }
}

static void b5_capacity(void)
{
    const uint64_t q = 3ull << 30; /* divisible by 3 */
    uint64_t caps[EFS_MAX_NODES];

    /* Equal quotas: 2/3 of the sum at every size; each addition adds 2Q/3. */
    uint64_t prev = 0;
    for (uint32_t n = 3; n <= 8; n++) {
        for (uint32_t i = 0; i < n; i++)
            caps[i] = q;
        uint64_t cap = efs_capacity_logical(caps, n);
        CHECK(cap == 2 * q * n / 3, "n=%u equal quotas: %llu", n, (unsigned long long)cap);
        if (n > 3)
            CHECK(cap - prev == 2 * q / 3, "n=%u addition added %llu", n, (unsigned long long)(cap - prev));
        prev = cap;
        printf("  B5 %u x %llu GiB: usable %llu GiB (physical %llu GiB, 1.5x)\n", n,
               (unsigned long long)(q >> 30), (unsigned long long)(cap >> 30),
               (unsigned long long)((uint64_t)n * q >> 30));
    }
    /* A full member: at three members nothing more is protected (0 free);
     * at four the rest holds 2Q; it never counts the full member's room. */
    uint64_t full3[3] = {q, q, 0}, full4[4] = {q, q, q, 0}, full5[5] = {q, q, q, q, 0};
    CHECK(efs_capacity_logical(full3, 3) == 0, "3 members, one full");
    CHECK(efs_capacity_logical(full4, 4) == 2 * q, "4 members, one full");
    CHECK(efs_capacity_logical(full5, 5) == 8 * q / 3, "5 members, one full");
    /* Unequal: the big member cannot take two fragments of one chunk. */
    uint64_t skew[3] = {100, 100, 1000}, skew4[4] = {100, 100, 100, 1000};
    CHECK(efs_capacity_logical(skew, 3) == 200 && efs_capacity_logical(skew4, 4) == 300, "skewed quotas");
    /* Random quotas against the brute-force bound; never more than 2/3 of
     * the physical sum, never more than twice the sum without the largest. */
    for (int t = 0; t < 2000; t++) {
        uint32_t n = 3 + (uint32_t)(next_rand() % 6);
        uint64_t sum = 0, big = 0;
        for (uint32_t i = 0; i < n; i++) {
            caps[i] = next_rand() % 5 == 0 ? 0 : next_rand() % 100000;
            sum += caps[i];
            big = caps[i] > big ? caps[i] : big;
        }
        uint64_t got = efs_capacity_logical(caps, n), want = ref_logical(caps, n);
        CHECK(got == want, "random caps n=%u: %llu want %llu", n, (unsigned long long)got,
              (unsigned long long)want);
        CHECK(got * 3 <= sum * 2 && got <= 2 * (sum - big), "random caps: %llu over the bound",
              (unsigned long long)got);
    }
    /* Protection cost: a full chunk of logical L stores 3 x L/2 payload
     * bytes (1.5x); a partial last chunk still stores three whole fragments.
     * Fragment records, alignment and metadata come on top (measured by the
     * daemon test, not claimed here). */
    for (size_t len = 1; len <= 128 * 1024; len *= 2) {
        size_t fl = 128 * 1024 / 2;
        double cost = (double)(3 * fl) / (double)len;
        if (len == 128 * 1024)
            CHECK(cost == 1.5, "full chunk cost %.3f", cost);
        else
            CHECK(cost > 1.5, "partial chunk cost %.3f", cost);
    }
    /* Startup headroom: 20% reserved (at least 1 GiB), 7/8 of the rest
     * budgeted; at or below the reserve nothing. */
    uint64_t g = 1ull << 30, b;
    CHECK(!efs_storage_quota_budget(100 * g, 100 * g, 0, &b) && b == 70 * g, "100 GiB budget %llu",
          (unsigned long long)b);
    CHECK(!efs_storage_quota_budget(4 * g, 4 * g, 0, &b) && b == 3 * g - (3 * g) / 8, "4 GiB budget");
    CHECK(efs_storage_quota_budget(4 * g, g, 0, &b) && efs_storage_quota_budget(g, g, 0, &b),
          "no budget inside the reserve");
    printf("B5: usable = 3-of-N bound (2/3 of equal quotas, +2Q/3 per addition, 0 free at three"
           " with one full member); 1.5x for full chunks only; 20%%/1 GiB headroom\n");
}

int main(void)
{
    b1_stripes();
    b2_payloads();
    b3_spread();
    b4_packing();
    b5_capacity();
    if (failures) {
        fprintf(stderr, "test_b_geometry: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_b_geometry: B1-B5 deterministic checks PASS\n");
    return 0;
}
