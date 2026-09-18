#include "efs/placement.h"
#include <stdio.h>

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
        {3737533, "10.1.223.57", 1981, "", 0, 0, 0, 0},
        {3803069, "10.1.223.58", 1981, "", 0, 0, 0, 0},
        {3868605, "10.1.223.59", 1981, "", 0, 0, 0, 0},
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
        {3, "10.1.223.57", 7740, "", 0, 0, 0, 0},
        {4, "10.1.223.58", 7740, "", 0, 0, 0, 0},
        {5, "10.1.223.59", 7740, "", 0, 0, 0, 0},
        {6, "10.1.223.60", 7740, "", 0, 0, 0, 0},
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
        {1, "node1", 7432, "", 0, 0, 0, 0},
        {2, "node2", 7432, "", 0, 0, 0, 0},
        {3, "localhost", 7432, "", 0, 0, 0, 0},
    };
    if (!efs_is_local_node(cluster, 3, "localhost", &local_id) || local_id != 3) {
        fprintf(stderr, "FAIL locality detection for localhost\n");
        failures++;
    }

    if (failures == 0) {
        printf("test_placement: OK\n");
        return 0;
    }
    printf("test_placement: %d failures\n", failures);
    return 1;
}
