#ifndef EFS_SERVER_INTERNAL_H
#define EFS_SERVER_INTERNAL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include <pthread.h>
#include <sched.h>

#define EFS_SERVER_MAX_CONNS 64
#define EFS_DEFAULT_WRITERS  16
#define EFS_MAX_WRITERS      64

enum efsd_server_state {
    SERVER_STATE_ACTIVE = 0,
    SERVER_STATE_LEAVING = 1,   /* leave cluster after successful remove (no migrate) */
    SERVER_STATE_SHRINKING = 2, /* reducing local quota usage */
    SERVER_STATE_DRAINING = 3,  /* migrating local fragments to peers */
    SERVER_STATE_DRAINED = 4,   /* empty; rejects new PUTs until undrain or remove */
};

struct efsd_server {
    efs_node_id_t id;
    char addr[64];
    uint16_t port;
    /* Local data roots: 1 (plain) or 3..EFS_MAX_STORAGE_PATHS (local EC).
     * storage_path always mirrors storage_paths[0] for advertise/compat. */
    char storage_paths[EFS_MAX_STORAGE_PATHS][EFS_MAX_PATH];
    uint32_t storage_path_count;
    char storage_path[EFS_MAX_PATH];
    /* Soft NUMA preference per storage path (-1 = unknown / unbound). */
    int storage_numa_node[EFS_MAX_STORAGE_PATHS];
    cpu_set_t storage_cpu_set[EFS_MAX_STORAGE_PATHS];
    int storage_affinity_valid[EFS_MAX_STORAGE_PATHS];
    uint64_t quota; /* local storage quota in bytes; 0 = unlimited */

    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t node_count;

    struct efs_export exports[EFS_MAX_EXPORTS];
    uint32_t export_count;
    uint32_t epoch;

    pthread_mutex_t lock;
    int listen_fd;
    int running;

    int state; /* enum efsd_server_state */
    uint64_t shrink_target; /* target used bytes after shrink-quota migration */
    pthread_t migrate_tid;
    pthread_t rejoin_tid; /* background rejoin retry thread */
    char rejoin_addr[64]; /* explicit --join target to keep retrying; empty = use persisted peers */
    uint16_t rejoin_port;

    int direct_io; /* use O_DIRECT for fragment reads/writes */
    int nwriters; /* dedicated fragment-writer threads (0 = inline) */
    int persist_nodes; /* persist cluster membership to disk */
    int perf; /* run under perf record when starting */
    int export_meta_dirty; /* defer metadata.bin writes across PUT_META */
};

/* Global server instance used by worker threads. */
extern struct efsd_server *g_server;

/* Find or create an export by name. */
struct efs_export *server_find_export(struct efsd_server *s, const char *name);

/* Get export by id. */
struct efs_export *server_get_export(struct efsd_server *s, efs_export_id_t id);

/* Migrate pre-subdirectory storage layout to data/meta/log. */
void server_migrate_old_layout(struct efsd_server *s);

/* Load all exports from storage path. */
void server_load_exports(struct efsd_server *s);

/* Save an export to disk. */
void server_save_export(struct efsd_server *s, struct efs_export *ex);

/* Get the path for a fragment on disk. */
int server_fragment_path(struct efsd_server *s, struct efs_export *ex,
                         efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                         char *path, size_t path_len);

/* Read a fragment from disk. */
int server_read_fragment(struct efsd_server *s, struct efs_export *ex,
                         efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                         uint8_t *data, uint32_t *data_len);

/* Synchronous fragment write (disk I/O); used by the writer pool. */
int server_write_fragment_sync(struct efsd_server *s, struct efs_export *ex,
                               efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                               const uint8_t *data, uint32_t data_len);

/* Queue fragment write onto the writer pool (or run inline if pool is off). */
int server_write_fragment(struct efsd_server *s, struct efs_export *ex,
                          efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                          const uint8_t *data, uint32_t data_len);

/* Fragment data + checksum sidecar as one pooled job. */
int server_write_fragment_with_sum(struct efsd_server *s, struct efs_export *ex,
                                   efs_ino_t ino, uint32_t chunk_index,
                                   uint32_t fragment_index,
                                   const uint8_t *data, uint32_t data_len,
                                   const uint8_t checksum[EFS_HASH_SIZE]);

int server_writer_pool_start(struct efsd_server *s);
void server_writer_pool_stop(struct efsd_server *s);

/* Compute total bytes used under one storage root's data/. */
uint64_t server_compute_usage(const char *path);

/* Logical used across all local roots (accounts for local EC overhead). */
uint64_t server_compute_local_usage(struct efsd_server *s);

/* Update the local node's used counter in the cluster list. */
void server_update_local_usage(struct efsd_server *s);

/* Pointer to this process's row in s->nodes (matched by s->id), or NULL. */
struct efs_node *server_local_node(struct efsd_server *s);

/* Return true if adding fragment_size bytes would exceed the server's quota. */
bool server_would_exceed_quota(struct efsd_server *s, uint64_t fragment_size);

/* Handle one client connection. */
void server_handle_conn(int fd);

/* Join an existing cluster by contacting a peer. */
int server_join_cluster(struct efsd_server *s, const char *peer_host, uint16_t peer_port);

/* Persist export as 2+1 meta pages + EFSR root; push root to peers. */
int server_flush_fragmented_meta(struct efsd_server *s, struct efs_export *ex);

/* Rebuild in-memory export tables from meta pages referenced by ex->root. */
int server_rebuild_export_from_pages(struct efsd_server *s, struct efs_export *ex);

/* After membership is known, rebuild any EFSR exports from meta pages. */
void server_rebuild_fragmented_exports(struct efsd_server *s);

/* Send metadata to all peers. Returns number of acks. */
int server_replicate_metadata(struct efsd_server *s, struct efs_export *ex);

/* Send a metadata snapshot (EFSR root or legacy EFSM) to a peer. */
int server_send_metadata_to(struct efsd_server *s, struct efs_export *ex,
                              const char *host, uint16_t port);

/* Fetch metadata from a peer. */
int server_fetch_metadata_from(struct efsd_server *s, const char *host, uint16_t port);

/* Start the background heartbeat thread. */
void server_start_heartbeat(struct efsd_server *s);

/* Start the background data migration thread. */
void server_start_migration(struct efsd_server *s);

/* Start the background cluster rejoin retry thread. */
void server_start_rejoin(struct efsd_server *s);

/* Remove a node from the local cluster list. */
void server_remove_node_from_cluster(struct efsd_server *s, efs_node_id_t node_id);

/* Broadcast a node-left notification to all peers. */
void server_notify_node_left(struct efsd_server *s, efs_node_id_t node_id);

/* True if any chunk still places a fragment on this node's id. Caller holds lock. */
int server_has_local_fragments_locked(struct efsd_server *s);

/* Rebuild meta maps + refresh used; return 1 if no local fragments and used==0. */
int server_node_is_empty(struct efsd_server *s);

/* Begin leave: notify peers and stop accepting connections. Caller holds lock. */
void server_begin_leave_locked(struct efsd_server *s);

/* Persist / load a fragment's blake3 checksum sidecar (written once at PUT). */
int server_write_fragment_sum_sync(struct efsd_server *s, struct efs_export *ex,
                                   efs_ino_t ino, uint32_t chunk_index,
                                   uint32_t fragment_index,
                                   const uint8_t checksum[EFS_HASH_SIZE]);
int server_write_fragment_sum(struct efsd_server *s, struct efs_export *ex,
                              efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                              const uint8_t checksum[EFS_HASH_SIZE]);
int server_read_fragment_sum(struct efsd_server *s, struct efs_export *ex,
                             efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                             uint8_t checksum[EFS_HASH_SIZE]);

/* Persist the cluster nodes list to disk. */
void server_save_nodes(struct efsd_server *s);

/* Load the persisted cluster nodes list from disk. */
void server_load_nodes(struct efsd_server *s);

/* Try to rejoin the cluster using any persisted peer. Returns 0 on success. */
int server_rejoin_cluster(struct efsd_server *s);

#endif
