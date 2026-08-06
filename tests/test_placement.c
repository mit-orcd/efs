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

    efs_node_id_t local_id;
    struct efs_node cluster[3] = {
        {1, "node1", 7432, "", 0, 0},
        {2, "node2", 7432, "", 0, 0},
        {3, "localhost", 7432, "", 0, 0},
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
