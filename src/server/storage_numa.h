#ifndef EFS_STORAGE_NUMA_H
#define EFS_STORAGE_NUMA_H

#include "efs/common.h"

struct efsd_server;

/* Discover NUMA affinity for every storage_paths[] entry on s. Soft-fail. */
void server_discover_storage_numa(struct efsd_server *s);

/* Apply best-effort affinity for storage_paths[path_index] to the current thread. */
void server_apply_storage_affinity(struct efsd_server *s, uint32_t path_index);

#endif
