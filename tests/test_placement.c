#include "efs/placement.h"
#include "efs/raft.h"
#include <stdio.h>
#include <string.h>

/* K28/W151: ring placement over n members (ids 1..n) for n = 5..max: three
 * distinct members per stripe, every member used across many inodes, and
 * one file's successive chunks rotate (the start walks with chunk_index). */
static int k28_placement(uint32_t n)
{
    int failures = 0;
    struct efs_node ring[EFS_MAX_NODES];
    uint32_t hits[EFS_MAX_NODES + 1][EFS_NUM_FRAGMENTS];
    efs_node_id_t f[EFS_NUM_FRAGMENTS], prev[EFS_NUM_FRAGMENTS] = {0};

    memset(ring, 0, sizeof(ring));
    memset(hits, 0, sizeof(hits));
    /* Membership order is not ring order: list ids descending. */
    for (uint32_t i = 0; i < n; i++)
        ring[i].id = (efs_node_id_t)(n - i);
    for (efs_ino_t ino = 1; ino <= 400; ino++) {
        for (uint32_t ci = 0; ci < 16; ci++) {
            efs_place_fragments(ring, n, ino, ci, f);
            for (int k = 0; k < EFS_NUM_FRAGMENTS; k++) {
                if (f[k] < 1 || f[k] > n) {
                    fprintf(stderr, "FAIL k28 n=%u node %u out of range\n", n, f[k]);
                    return failures + 1;
                }
                hits[f[k]][k]++;
            }
            if (f[0] == f[1] || f[0] == f[2] || f[1] == f[2]) {
                fprintf(stderr, "FAIL k28 n=%u ino=%llu ci=%u duplicate (%u,%u,%u)\n",
                        n, (unsigned long long)ino, ci, f[0], f[1], f[2]);
                failures++;
            }
            /* Rotation: the next chunk of the same file moves its start. */
            if (ci && f[0] == prev[0]) {
                fprintf(stderr, "FAIL k28 n=%u ino=%llu ci=%u did not rotate\n",
                        n, (unsigned long long)ino, ci);
                failures++;
            }
            memcpy(prev, f, sizeof(prev));
        }
    }
    /* Per-node load stays within 25% of the mean for every fragment slot. */
    for (uint32_t id = 1; id <= n; id++) {
        for (int k = 0; k < EFS_NUM_FRAGMENTS; k++) {
            uint32_t mean = 400u * 16u / n;
            if (!hits[id][k] || hits[id][k] < mean * 3 / 4 || hits[id][k] > mean * 5 / 4) {
                fprintf(stderr, "FAIL k28 n=%u node %u fragment %d load %u (mean %u)\n",
                        n, id, k, hits[id][k], mean);
                failures++;
            }
        }
    }
    /* A file of n chunks touches every member. */
    {
        int seen[EFS_MAX_NODES + 1] = {0}, all = 1;
        for (uint32_t ci = 0; ci < n; ci++) {
            efs_place_fragments(ring, n, 77, ci, f);
            for (int k = 0; k < EFS_NUM_FRAGMENTS; k++) seen[f[k]] = 1;
        }
        for (uint32_t id = 1; id <= n; id++) all &= seen[id];
        if (!all) {
            fprintf(stderr, "FAIL k28 n=%u one file did not touch every member\n", n);
            failures++;
        }
    }
    /* Ring is keyed by sorted ids: arbitrary ids map to the same ranks. */
    {
        struct efs_node sparse[EFS_MAX_NODES];
        efs_node_id_t g[EFS_NUM_FRAGMENTS];
        memset(sparse, 0, sizeof(sparse));
        for (uint32_t i = 0; i < n; i++) sparse[i].id = (efs_node_id_t)(1000 + 7 * (i + 1));
        for (uint32_t ci = 0; ci < 32; ci++) {
            efs_place_fragments(ring, n, 99, ci, f);
            efs_place_fragments(sparse, n, 99, ci, g);
            for (int k = 0; k < EFS_NUM_FRAGMENTS; k++)
                if (g[k] != 1000 + 7 * f[k]) {
                    fprintf(stderr, "FAIL k28 n=%u sparse ids diverge\n", n);
                    failures++;
                    ci = 32;
                    break;
                }
        }
    }
    /* Capacity: n equal nodes of C give 2nC/3; one huge node is capped. */
    {
        uint64_t caps[EFS_MAX_NODES];
        const uint64_t C = 300ull << 30;
        for (uint32_t i = 0; i < n; i++) caps[i] = C;
        uint64_t got = efs_capacity_logical(caps, n), want = 2 * (n * C / 3);
        if (got + 2 < want || got > want + 2) {
            fprintf(stderr, "FAIL k28 capacity n=%u got %llu want %llu\n", n,
                    (unsigned long long)got, (unsigned long long)want);
            failures++;
        }
        /* One node with 100x: M = (n-1)C/2 when (n-1)C >= 2M, so 2M = (n-1)C. */
        caps[0] = 100 * C;
        got = efs_capacity_logical(caps, n);
        want = (uint64_t)(n - 1) * C;
        if (got + 2 < want || got > want + 2) {
            fprintf(stderr, "FAIL k28 capacity skew n=%u got %llu want %llu\n", n,
                    (unsigned long long)got, (unsigned long long)want);
            failures++;
        }
        /* An empty member is not counted. */
        caps[0] = 0;
        got = efs_capacity_logical(caps, n);
        want = 2 * ((uint64_t)(n - 1) * C / 3);
        if (got + 2 < want || got > want + 2) {
            fprintf(stderr, "FAIL k28 capacity empty n=%u got %llu want %llu\n", n,
                    (unsigned long long)got, (unsigned long long)want);
            failures++;
        }
    }
    return failures;
}

/* K28/W151 (D38): packed segments use the data ring, not the voters: three
 * distinct live members per segment, non-voters (nodes 5..n) included, a
 * down member never, self always live, and fewer than three live members
 * refused. Deterministic in the segment id and the membership set. */
static int k28_pack_destinations(uint32_t n)
{
    int failures = 0;
    struct efs_node m[EFS_MAX_NODES], rev[EFS_MAX_NODES];
    uint32_t used[EFS_MAX_NODES + 1] = {0}, with_nonvoter = 0;
    efs_node_id_t d[EFS_NUM_FRAGMENTS], e[EFS_NUM_FRAGMENTS];
    const efs_node_id_t self = 1, down = 3; /* node 3 votes in both groups */
    const uint32_t voters = efs_md_group_voters(EFS_RAFT_GROUP_SHARD, (int)n) |
                            efs_md_group_voters(EFS_RAFT_GROUP_SHARD2, (int)n);

    memset(m, 0, sizeof(m));
    for (uint32_t i = 0; i < n; i++) m[i].id = (efs_node_id_t)(i + 1);
    m[down - 1].down_until_ms = 12345;
    m[self - 1].down_until_ms = 1; /* stale own row: self still counts */
    for (uint32_t i = 0; i < n; i++) rev[i] = m[n - 1 - i];
    for (uint64_t t = 1; t <= 3000; t++) {
        uint64_t id = (t * 1000003ull) << 8 | (t % n + 1);
        if (efs_pack_destinations(m, n, self, id, d) != EFS_OK) {
            fprintf(stderr, "FAIL k28 pack n=%u refused with %u live\n", n, n - 1);
            return failures + 1;
        }
        if (efs_pack_destinations(rev, n, self, id, e) != EFS_OK || memcmp(d, e, sizeof(d))) {
            fprintf(stderr, "FAIL k28 pack n=%u depends on membership order\n", n);
            failures++;
        }
        int nv = 0;
        for (int k = 0; k < EFS_NUM_FRAGMENTS; k++) {
            if (!d[k] || d[k] > n || d[k] == down) {
                fprintf(stderr, "FAIL k28 pack n=%u bad destination %u\n", n, d[k]);
                failures++;
                continue;
            }
            used[d[k]]++;
            nv += !(voters >> (d[k] - 1) & 1u);
        }
        if (d[0] == d[1] || d[0] == d[2] || d[1] == d[2]) {
            fprintf(stderr, "FAIL k28 pack n=%u duplicate destination\n", n);
            failures++;
        }
        with_nonvoter += nv > 0;
    }
    for (uint32_t id = 1; id <= n; id++) {
        if ((id == down) != !used[id]) {
            fprintf(stderr, "FAIL k28 pack n=%u node %u used %u times\n", n, id, used[id]);
            failures++;
        }
    }
    /* Every non-voter (nodes 5..n) carries fragments: checked above via
     * used[]; most segments include at least one once n >= 5. */
    if (!with_nonvoter || (n >= 8 && with_nonvoter < 3000 / 2)) {
        fprintf(stderr, "FAIL k28 pack n=%u too few segments on non-voters (%u)\n", n,
                with_nonvoter);
        failures++;
    }
    /* Three down of five live-capable members leaves two: refused. */
    {
        struct efs_node f[5];
        memset(f, 0, sizeof(f));
        for (uint32_t i = 0; i < 5; i++) f[i].id = i + 1;
        f[1].down_until_ms = f[2].down_until_ms = f[3].down_until_ms = 1;
        d[0] = d[1] = d[2] = 9;
        if (efs_pack_destinations(f, 5, 1, 42, d) != EFS_ERR_BUSY || d[0] || d[1] || d[2]) {
            fprintf(stderr, "FAIL k28 pack accepted two live members\n");
            failures++;
        }
        /* Duplicate and out-of-range ids never count as members. */
        f[1].down_until_ms = f[2].down_until_ms = f[3].down_until_ms = 0;
        f[1].id = 1; f[2].id = 0; f[3].id = EFS_MAX_NODES + 1;
        if (efs_pack_destinations(f, 5, 1, 42, d) != EFS_ERR_BUSY) {
            fprintf(stderr, "FAIL k28 pack counted duplicate/invalid ids\n");
            failures++;
        }
        f[3].id = 4;
        if (efs_pack_destinations(f, 5, 1, 42, d) != EFS_OK ||
            efs_pack_destinations(NULL, 5, 1, 42, d) != EFS_ERR_BUSY) {
            fprintf(stderr, "FAIL k28 pack three-member edge\n");
            failures++;
        }
    }
    return failures;
}

/* K28/W151 + D38: three voters per group for n >= 4, nodes 5..n host no
 * metadata, n <= 3 all nodes; every set is a valid Raft voter mask. */
static int k28_voters(void)
{
    int failures = 0;
    for (int n = 1; n <= EFS_MAX_NODES; n++) {
        uint32_t a = efs_md_group_voters(EFS_RAFT_GROUP_SHARD, n);
        uint32_t b = efs_md_group_voters(EFS_RAFT_GROUP_SHARD2, n);
        uint32_t wa = n <= 3 ? (1u << n) - 1u : 0x7u;
        uint32_t wb = n <= 3 ? (1u << n) - 1u : 0xeu;
        if (a != wa || b != wb || !efs_raft_voters_valid(a) || !efs_raft_voters_valid(b) ||
            (n >= 3 && (__builtin_popcount(a) != 3 || __builtin_popcount(b) != 3)) ||
            (a | b) >> (n < 4 ? n : 4)) {
            fprintf(stderr, "FAIL k28 voters n=%d shard=%#x shard2=%#x\n", n, a, b);
            failures++;
        }
    }
    if (efs_md_group_voters(EFS_RAFT_GROUP_SHARD, 0) ||
        efs_md_group_voters(EFS_RAFT_GROUP_SHARD, EFS_MAX_NODES + 1)) {
        fprintf(stderr, "FAIL k28 voters out-of-range n accepted\n");
        failures++;
    }
    /* Every member id is a valid Raft peer (replacement may land anywhere). */
    if (EFS_RAFT_MAX_PEERS != EFS_MAX_NODES ||
        !efs_raft_voters_valid(1u << (EFS_MAX_NODES - 1)) ||
        efs_raft_voters_valid(1u << EFS_MAX_NODES)) {
        fprintf(stderr, "FAIL k28 raft peer space != cluster cap\n");
        failures++;
    }
    return failures;
}

int main(void)
{
    int failures = 0;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];

    efs_get_placement(3, 1, 0, nodes);
    if (nodes[0] < 1 || nodes[0] > 3 || nodes[1] < 1 || nodes[1] > 3 || nodes[2] < 1 || nodes[2] > 3) {
        fprintf(stderr, "FAIL placement out of range for ino=1 chunk=0\n");
        failures++;
    }

    int seen[4][3] = {{0}};
    for (efs_ino_t ino = 1; ino <= 100; ino++) {
        for (uint32_t ci = 0; ci < 50; ci++) {
            efs_get_placement(3, ino, ci, nodes);
            for (int i = 0; i < 3; i++) {
                if (nodes[i] < 1 || nodes[i] > 3) {
                    failures++;
                } else {
                    seen[nodes[i]][i]++;
                }
            }
        }
    }

    for (int n = 1; n <= 3; n++) {
        for (int f = 0; f < 3; f++) {
            if (seen[n][f] == 0) {
                fprintf(stderr, "FAIL node %d never got fragment %d\n", n, f);
                failures++;
            }
        }
    }

    /* Real clusters use non-sequential ids (e.g. derived from IP:port). */
    struct efs_node cluster_big[3] = {
        {3737533, "10.1.223.57", 1981, "", 0, 0, 0, 0, 0},
        {3803069, "10.1.223.58", 1981, "", 0, 0, 0, 0, 0},
        {3868605, "10.1.223.59", 1981, "", 0, 0, 0, 0, 0},
    };
    for (efs_ino_t ino = 1; ino <= 20; ino++) {
        efs_place_fragments(cluster_big, 3, ino, 0, nodes);
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            int ok = 0;
            for (int j = 0; j < 3; j++) {
                if (nodes[i] == cluster_big[j].id)
                    ok = 1;
            }
            if (!ok) {
                fprintf(stderr,
                        "FAIL place_fragments returned unknown id %u "
                        "(ino=%llu frag=%d)\n",
                        nodes[i], (unsigned long long)ino, i);
                failures++;
            }
        }
        /* Distinct hosts for 2+1. */
        if (nodes[0] == nodes[1] || nodes[0] == nodes[2] ||
            nodes[1] == nodes[2]) {
            fprintf(stderr, "FAIL place_fragments duplicated nodes for ino=%llu\n",
                    (unsigned long long)ino);
            failures++;
        }
    }

    /* 4-node cluster: one file's chunks must use all servers, never two
     * fragments of a chunk on one node, and no node should hog the file. */
    struct efs_node cluster4[4] = {
        {3, "10.1.223.57", 7740, "", 0, 0, 0, 0, 0},
        {4, "10.1.223.58", 7740, "", 0, 0, 0, 0, 0},
        {5, "10.1.223.59", 7740, "", 0, 0, 0, 0, 0},
        {6, "10.1.223.60", 7740, "", 0, 0, 0, 0, 0},
    };
    int hit[7] = {0};
    int load[7] = {0};
    efs_ino_t file = 42;
    for (uint32_t ci = 0; ci < 8; ci++) {
        efs_place_fragments(cluster4, 4, file, ci, nodes);
        if (nodes[0] == nodes[1] || nodes[0] == nodes[2] ||
            nodes[1] == nodes[2]) {
            fprintf(stderr, "FAIL 4-node duplicate nodes chunk=%u\n", ci);
            failures++;
        }
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            hit[nodes[i]] = 1;
            load[nodes[i]]++;
        }
    }
    if (!hit[3] || !hit[4] || !hit[5] || !hit[6]) {
        fprintf(stderr,
                "FAIL 4-node file did not touch every server (3=%d 4=%d 5=%d 6=%d)\n",
                hit[3], hit[4], hit[5], hit[6]);
        failures++;
    }
    int lo = 1000, hi = 0;
    for (int id = 3; id <= 6; id++) {
        if (load[id] < lo)
            lo = load[id];
        if (load[id] > hi)
            hi = load[id];
    }
    /* 8 chunks × 3 frags / 4 nodes = 6 each if perfectly even. */
    if (hi - lo > 1) {
        fprintf(stderr, "FAIL 4-node file load uneven lo=%d hi=%d\n", lo, hi);
        failures++;
    }

    /* Meta placement must stay on the original consecutive hash. */
    efs_node_id_t meta_a[EFS_NUM_FRAGMENTS], meta_b[EFS_NUM_FRAGMENTS];
    efs_get_placement(4, EFS_META_TABLE_INO, 0, meta_a);
    efs_get_placement(4, EFS_META_TABLE_INO, 0, meta_b);
    if (meta_a[0] != meta_b[0] || meta_a[1] != meta_b[1] ||
        meta_a[2] != meta_b[2]) {
        fprintf(stderr, "FAIL meta placement not stable\n");
        failures++;
    }
    if ((meta_a[1] != (meta_a[0] % 4) + 1 &&
         !(meta_a[0] == 4 && meta_a[1] == 1)) ||
        (meta_a[2] != (meta_a[1] % 4) + 1 &&
         !(meta_a[1] == 4 && meta_a[2] == 1))) {
        fprintf(stderr, "FAIL meta ranks not consecutive (%u,%u,%u)\n",
                meta_a[0], meta_a[1], meta_a[2]);
        failures++;
    }

    efs_node_id_t local_id;
    struct efs_node cluster[3] = {
        {1, "node1", 7432, "", 0, 0, 0, 0, 0},
        {2, "node2", 7432, "", 0, 0, 0, 0, 0},
        {3, "localhost", 7432, "", 0, 0, 0, 0, 0},
    };
    if (!efs_is_local_node(cluster, 3, "localhost", &local_id) || local_id != 3) {
        fprintf(stderr, "FAIL locality detection for localhost\n");
        failures++;
    }

    /* W42: usable logical capacity under 3-of-N fragment placement. */
    {
        const uint64_t G = 1ull << 30;
        struct {
            const char *name;
            uint64_t caps[6];
            uint32_t n;
            uint64_t want;
        } cases[] = {
            { "3 equal 200G → 400G", { 200 * G, 200 * G, 200 * G }, 3, 400 * G },
            { "4 equal 200G → Σ×2/3 = 533.33G", { 200 * G, 200 * G, 200 * G, 200 * G },
              4, (800 * G) * 2 / 3 },
            { "4 equal 500G → 1333.33G", { 500 * G, 500 * G, 500 * G, 500 * G },
              4, (2000 * G) * 2 / 3 },
            { "100/100/1000 → 200", { 100, 100, 1000 }, 3, 200 },
            { "100/100/100/1000 → 2×(300/2)=300", { 100, 100, 100, 1000 }, 4, 300 },
            { "6 equal 1 → 4", { 1, 1, 1, 1, 1, 1 }, 6, 4 },
            { "two nodes → 0", { 100, 100 }, 2, 0 },
            { "three nodes, one empty → 0", { 100, 100, 0 }, 3, 0 },
            { "four nodes, one empty → 3-node answer", { 100, 100, 0, 100 }, 4, 200 },
            { "one node free → 0", { 0, 0, 100, 0 }, 4, 0 },
        };
        size_t ci;

        for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
            uint64_t got = efs_capacity_logical(cases[ci].caps, cases[ci].n);
            /* Σ×2/3 truncates differently from 2×floor(Σ/3): allow 2 bytes. */
            uint64_t d = got > cases[ci].want ? got - cases[ci].want
                                              : cases[ci].want - got;
            if (d > 2) {
                fprintf(stderr, "FAIL capacity %s: got %llu want %llu\n",
                        cases[ci].name, (unsigned long long)got,
                        (unsigned long long)cases[ci].want);
                failures++;
            }
        }
        /* The bound is tight: one more fragment-byte per node-slot does
         * not fit. 100/100/1000 at M=101 needs 303 and has 301. */
        if (efs_capacity_logical((const uint64_t[]){ 100, 100, 1000 }, 3) != 200) {
            fprintf(stderr, "FAIL capacity tightness\n");
            failures++;
        }
    }

    {
        static const uint32_t ks[] = {5, 6, 7, 8, 9, 12, EFS_MAX_NODES};
        for (size_t i = 0; i < sizeof(ks) / sizeof(ks[0]); i++)
            failures += k28_placement(ks[i]);
        failures += k28_voters();
        for (size_t i = 0; i < sizeof(ks) / sizeof(ks[0]); i++)
            failures += k28_pack_destinations(ks[i]);
        if (EFS_MAX_NODES != 16) {
            fprintf(stderr, "FAIL k28 cap changed: update the K28 evidence summary\n");
            failures++;
        }
    }

    if (failures == 0) {
        printf("test_placement: OK\n");
        return 0;
    }
    printf("test_placement: %d failures\n", failures);
    return 1;
}
