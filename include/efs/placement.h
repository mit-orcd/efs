#ifndef EFS_PLACEMENT_H
#define EFS_PLACEMENT_H

#include "efs/common.h"

/* Rank-space placement: fragment_nodes[0..2] are 1-based ranks in
 * 1..node_count (not necessarily real efs_node_id_t values). Prefer
 * efs_place_fragments() for live clusters whose node ids are arbitrary.
 *
 * User data: 3 distinct ranks, spaced around the ring, walking by chunk
 * so one file does not pile onto the same 3-node window; the start is a
 * full-avalanche hash of the inode, so inode numbers that differ only in
 * high bits (one shard steps by 2^shard_bits) still spread. Metadata
 * pages (EFS_META_TABLE_INO) use consecutive ranks from the same hash.
 * New writes only: readers use the fragment nodes recorded in the chunk
 * table, so a placement change never moves existing data. */
void efs_get_placement(uint32_t node_count, efs_ino_t ino, uint32_t chunk_index,
                       efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS]);

/* Place fragments onto the live membership list. Members are ordered by
 * ascending node id to form a stable ring; fragment_nodes[] receives the
 * real efs_node_id_t values from `nodes`. */
void efs_place_fragments(const struct efs_node *nodes, uint32_t node_count,
                         efs_ino_t ino, uint32_t chunk_index,
                         efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS]);

/* K28/W151 (D38): fragment destinations of a packed segment. Packed
 * segments are data, so they use the whole data ring, not the group's
 * voters: three distinct live members chosen by efs_place_fragments'
 * ring keyed by segment_id (successive segments rotate over all N).
 * Live = an id in 1..EFS_MAX_NODES that the heartbeat has not marked down
 * (self always counts as up). Returns EFS_ERR_BUSY, out[] zeroed, with
 * fewer than EFS_NUM_FRAGMENTS live members. */
int efs_pack_destinations(const struct efs_node *members, uint32_t count,
                          efs_node_id_t self, uint64_t segment_id,
                          efs_node_id_t out[EFS_NUM_FRAGMENTS]);

/* B1: a recorded stripe is usable only as exactly three non-zero, distinct
 * node ids in 1..EFS_MAX_NODES (D1, D2, P). Readers take fragment nodes from
 * the chunk row or pack object and refuse anything else; they never
 * recompute placement. Returns 1 when valid, 0 otherwise. */
int efs_fragment_nodes_valid(const efs_node_id_t nodes[EFS_NUM_FRAGMENTS]);

/* A hashed index in [0, n) over the pair (a, b): every input bit moves
 * every output bit, reduced without modulo. 0 when n == 0. */
uint32_t efs_place_index(uint64_t a, uint64_t b, uint32_t n);

/* Spare for stripe slot `slot` whose member is down: one of `live` that is
 * not already in stripe[] (any slot), chosen by hash of (ino, chunk_index,
 * slot) over the candidates in ascending id order, so displaced fragments
 * spread over every eligible member instead of the first in the list.
 * Returns 0 when no candidate is left. */
efs_node_id_t efs_place_spare(const efs_node_id_t *live, uint32_t count,
                              efs_ino_t ino, uint32_t chunk_index, int slot,
                              const efs_node_id_t stripe[EFS_NUM_FRAGMENTS]);

/* Logical bytes that fit on nodes with per-node fragment capacities
 * caps[0..n-1] (bytes each node can still take, or its quota), given 2+1
 * EC: every 128 KiB chunk is three 64 KiB fragments on three DISTINCT
 * nodes, so no node ever holds more than half the logical bytes. The
 * answer is 2 × M for the largest M with Σ min(caps[i], M) ≥ 3 M:
 * three equal quotas Q → 2Q, N equal quotas → Σ × 2/3, 100/100/1000 →
 * 200 (the big node cannot take a second fragment of the same chunk).
 * Fewer than three nodes, or no capacity, → 0. Used by `df` (statfs)
 * and `efs-mgmt status` — one model for total and for free (W42). */
uint64_t efs_capacity_logical(const uint64_t *caps, uint32_t n);

/* Return true if the given host string matches a known server node.
 * Used by the FUSE client for data locality.
 * comparison is case-insensitive on the address part. */
bool efs_is_local_node(const struct efs_node *nodes, uint32_t node_count,
                       const char *host, efs_node_id_t *local_id);

#endif
