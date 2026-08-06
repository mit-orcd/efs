#ifndef EFS_SERVER_INTERNAL_H
#define EFS_SERVER_INTERNAL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include <pthread.h>

#define EFS_SERVER_MAX_CONNS 64

enum efsd_server_state {
    SERVER_STATE_ACTIVE = 0,
    SERVER_STATE_LEAVING = 1,   /* draining data and exiting */
    SERVER_STATE_SHRINKING = 2, /* reducing local quota usage */
};

struct efsd_server {
    efs_node_id_t id;
    char addr[64];
    uint16_t port;
    char storage_path[EFS_MAX_PATH];
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

/* Write a fragment to disk. */
int server_write_fragment(struct efsd_server *s, struct efs_export *ex,
                          efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                          const uint8_t *data, uint32_t data_len);

/* Compute total bytes used in the server's storage path. */
uint64_t server_compute_usage(const char *path);

/* Update the local node's used counter in the cluster list. */
void server_update_local_usage(struct efsd_server *s);

/* Return true if adding fragment_size bytes would exceed the server's quota. */
bool server_would_exceed_quota(struct efsd_server *s, uint64_t fragment_size);

/* Handle one client connection. */
void server_handle_conn(int fd);

/* Join an existing cluster by contacting a peer. */
int server_join_cluster(struct efsd_server *s, const char *peer_host, uint16_t peer_port);

/* Send metadata to all peers. Returns number of acks. */
int server_replicate_metadata(struct efsd_server *s, struct efs_export *ex);

/* Send a full metadata snapshot to a peer. */
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

/* Persist / load a fragment's blake3 checksum sidecar (written once at PUT). */
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
