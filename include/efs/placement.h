#ifndef EFS_PLACEMENT_H
#define EFS_PLACEMENT_H

#include "efs/common.h"

/* Rank-space placement: fragment_nodes[0..2] are 1-based ranks in
 * 1..node_count (not necessarily real efs_node_id_t values). Prefer
 * efs_place_fragments() for live clusters whose node ids are arbitrary.
 *
 * User data: 3 distinct ranks, spaced around the ring, walking by chunk
 * so one file does not pile onto the same 3-node window. Metadata pages
 * (EFS_META_TABLE_INO) keep the original consecutive hash so existing
 * EFSR fragments stay findable. */
void efs_get_placement(uint32_t node_count, efs_ino_t ino, uint32_t chunk_index,
                       efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS]);

/* Place fragments onto the live membership list. Members are ordered by
 * ascending node id to form a stable ring; fragment_nodes[] receives the
 * real efs_node_id_t values from `nodes`. */
void efs_place_fragments(const struct efs_node *nodes, uint32_t node_count,
                         efs_ino_t ino, uint32_t chunk_index,
                         efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS]);

/* Return true if the given host string matches a known server node.
 * Used by the FUSE client for data locality.
 * comparison is case-insensitive on the address part. */
bool efs_is_local_node(const struct efs_node *nodes, uint32_t node_count,
                       const char *host, efs_node_id_t *local_id);

#endif
