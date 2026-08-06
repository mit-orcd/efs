#ifndef EFS_PLACEMENT_H
#define EFS_PLACEMENT_H

#include "efs/common.h"

/* Determine which nodes hold fragments 0,1,2 for (ino, chunk_index).
 * node_count is the current cluster size (<= EFS_MAX_NODES).
 * fragment_nodes[0..2] are returned as 1-based node IDs.
 */
void efs_get_placement(uint32_t node_count, efs_ino_t ino, uint32_t chunk_index,
                       efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS]);

/* Return true if the given host string matches a known server node.
 * Used by the FUSE client for data locality.
 * comparison is case-insensitive on the address part. */
bool efs_is_local_node(const struct efs_node *nodes, uint32_t node_count,
                       const char *host, efs_node_id_t *local_id);

#endif
