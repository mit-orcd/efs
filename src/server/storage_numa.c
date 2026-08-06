#include "storage_numa.h"
#include "server_internal.h"
#include "efs/numa_locality.h"
#include <stdio.h>
#include <string.h>

void server_discover_storage_numa(struct efsd_server *s)
{
    if (!s)
        return;
    uint32_t n = s->storage_path_count ? s->storage_path_count : 1;
    for (uint32_t i = 0; i < EFS_MAX_STORAGE_PATHS; i++) {
        s->storage_numa_node[i] = -1;
        s->storage_affinity_valid[i] = 0;
        CPU_ZERO(&s->storage_cpu_set[i]);
    }
    for (uint32_t i = 0; i < n; i++) {
        int node = -1;
        cpu_set_t set;
        CPU_ZERO(&set);
        if (efs_numa_for_path(s->storage_paths[i], &node, &set) == 0) {
            s->storage_numa_node[i] = node;
            s->storage_cpu_set[i] = set;
            s->storage_affinity_valid[i] = 1;
            char cpus[256];
            efs_numa_format_cpuset(&set, cpus, sizeof(cpus));
            printf("storage[%u]=%s numa=%d cpus=%s\n", i, s->storage_paths[i],
                   node, cpus);
        } else {
            printf("storage[%u]=%s numa=unknown (unbound)\n", i,
                   s->storage_paths[i]);
        }
    }
    fflush(stdout);
}

void server_apply_storage_affinity(struct efsd_server *s, uint32_t path_index)
{
    if (!s || path_index >= EFS_MAX_STORAGE_PATHS)
        return;
    if (!s->storage_affinity_valid[path_index])
        return;
    efs_numa_apply_affinity(&s->storage_cpu_set[path_index]);
}
