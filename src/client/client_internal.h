#ifndef EFS_CLIENT_INTERNAL_H
#define EFS_CLIENT_INTERNAL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include <pthread.h>
#include <sys/types.h>

struct efs_client {
    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t node_count;
    efs_node_id_t local_node_id;

    efs_export_id_t export_id;
    char export_name[EFS_MAX_NAME];
    struct efs_export export;
    pthread_mutex_t lock;

    /* Inode allocation namespace. When non-zero, new inodes are allocated as
     * (ino_namespace | counter) so that concurrent clients never assign the
     * same inode number to different files (which would collide chunk
     * placements and corrupt data). Zero means allocate sequentially from
     * export.next_ino (single-client / test behaviour). */
    uint64_t ino_namespace;
    uint64_t ino_counter;

    /* Persistent TCP connection pool to each server. The server's accept
     * loop handles multiple requests per connection; a pool of sockets lets
     * FUSE workers and parallel fragment PUTs proceed without serializing
     * on a single fd. Checkout waits if all slots are busy. */
    int conn_fd[EFS_MAX_NODES][EFS_CLIENT_CONNS_PER_NODE];
    int conn_busy[EFS_MAX_NODES][EFS_CLIENT_CONNS_PER_NODE];
    pthread_mutex_t conn_lock[EFS_MAX_NODES];
    pthread_cond_t conn_cv[EFS_MAX_NODES];
    int conn_pool_size; /* 1..EFS_CLIENT_CONNS_PER_NODE, from env or default */

    /* When non-zero, metadata replication is coalesced: changes only mark
     * dirty and flush every meta_batch_ops operations (or when forced on
     * unmount). Flushes send a delta of dirty inodes/chunks only (not the
     * full export) so bulk creates stay O(n). C tests leave this at 0 for
     * immediate full-metadata sync. */
    int meta_batch;
    uint32_t meta_batch_ops; /* flush threshold; 0 → default */
    uint32_t meta_dirty_ops;
    int meta_dirty;

    /* Dirty tracking for batched delta flushes (meta_batch only).
     * Inodes: open-addressing set (key 0 = empty).
     * Chunks: set for dedup + parallel arrays of (ino, chunk_index). */
    uint64_t *dirty_ino_keys;
    uint64_t dirty_ino_mask;
    uint64_t dirty_ino_count;
    uint64_t *dirty_chunk_keys;
    uint64_t dirty_chunk_mask;
    efs_ino_t *dirty_chunk_inos;
    uint32_t *dirty_chunk_idxs;
    uint64_t dirty_chunk_count;
    uint64_t dirty_chunk_cap;
};

/* Mark inode/chunk dirty for the next batched metadata delta flush.
 * No-ops when meta_batch is disabled. Caller must hold g_client.lock. */
void efs_client_mark_ino_dirty(efs_ino_t ino);
void efs_client_mark_chunk_dirty(efs_ino_t ino, uint32_t chunk_index);

extern struct efs_client g_client;

/* Resolve a path to an inode number. Returns 0 on success. */
int efs_client_lookup(const char *path, struct efs_inode *out);

/* Fetch full metadata from a server and replace local copy. */
int efs_client_fetch_metadata(const char *host, uint16_t port);

/* Replicate local metadata to all servers. Returns number of acks. */
/* Must be called without g_client.lock held; it takes the lock only to
 * serialize, then releases it for the duration of the network I/O. */
int efs_client_replicate_metadata(void);

/* Record a local metadata mutation. With meta_batch==0 this replicates
 * immediately (test / C-API behaviour). With meta_batch!=0 it coalesces
 * until meta_batch_ops changes accumulate (or force!=0). */
int efs_client_note_meta_change(int force);

/* Enable coalesced metadata replication for the FUSE client. */
void efs_client_enable_meta_batch(uint32_t every_n_ops);

/* Fetch a fragment from a node. Returns 0 on success. */
int efs_client_get_fragment(efs_node_id_t node_id, efs_ino_t ino, uint32_t chunk_index,
                            uint32_t fragment_index, uint8_t *data, uint32_t *data_len,
                            uint8_t checksum[EFS_HASH_SIZE]);

/* Store a fragment on a node. Returns 0 on success. */
int efs_client_put_fragment(efs_node_id_t node_id, efs_ino_t ino, uint32_t chunk_index,
                            uint32_t fragment_index, const uint8_t *data,
                            const uint8_t checksum[EFS_HASH_SIZE]);

/* Read bytes from a file. Returns 0 on success. */
int efs_client_read(efs_ino_t ino, uint64_t offset, size_t size, char *buf, size_t *out_len);

/* Write bytes to a file and replicate metadata. Returns 0 on success. */
int efs_client_write(efs_ino_t ino, uint64_t offset, size_t size, const char *buf);

/* Write bytes without replicating metadata. The caller must replicate later
   (e.g. on FUSE release). */
int efs_client_write_no_replicate(efs_ino_t ino, uint64_t offset, size_t size, const char *buf);

/* Create a file or directory. Returns the inode number or 0 on error. */
efs_ino_t efs_client_create(efs_ino_t parent, const char *name, uint32_t mode,
                            uid_t uid, gid_t gid);

/* Remove a file or directory. */
int efs_client_unlink(efs_ino_t parent, const char *name, bool is_dir);

/* Add a hard link to an existing inode under new_parent/new_name. */
int efs_client_link(efs_ino_t src_ino, efs_ino_t new_parent, const char *new_name);

/* Set mode bits. */
int efs_client_chmod(efs_ino_t ino, uint32_t mode);

/* Set owner/group. */
int efs_client_chown(efs_ino_t ino, uid_t uid, gid_t gid);

/* Set modification time. */
int efs_client_utime(efs_ino_t ino, uint64_t mtime);

/* Truncate or extend a file to the given size. */
int efs_client_truncate(efs_ino_t ino, uint64_t size);

/* Rename/move an inode to a new parent and name. */
int efs_client_rename(efs_ino_t ino, efs_ino_t new_parent, const char *new_name);

/* Initialize node cache from the list of nodes. */
void efs_client_init_nodes(struct efs_client *c, const char *node_list[EFS_MAX_NODES],
                           uint32_t node_count);

/* Discover the full cluster membership by asking one server. Returns 0 on success. */
int efs_client_discover_nodes(struct efs_client *c, const char *host, uint16_t port);

/* Borrow a live TCP fd for node_id (1-based). Blocks until a pool slot is
 * free. Caller must efs_client_conn_release(node_id, fd) or _drop(...). */
int efs_client_conn_get(efs_node_id_t node_id);

/* Return a healthy connection to the pool. */
void efs_client_conn_release(efs_node_id_t node_id, int fd);

/* Close a broken connection and free its pool slot. */
void efs_client_conn_drop(efs_node_id_t node_id, int fd);

/* Initialize the connection pool (call once after node_count is known). */
void efs_client_conn_init(void);

/* PUT all three fragments concurrently (send-all / recv-all on three
 * pooled sockets). Returns EFS_OK if quorum (>=2) acks, else an error. */
int efs_client_put_fragments_parallel(efs_ino_t ino, uint32_t chunk_index,
                                      const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                      const uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE],
                                      const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE]);

#endif
