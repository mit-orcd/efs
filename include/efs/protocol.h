#ifndef EFS_PROTOCOL_H
#define EFS_PROTOCOL_H

#include "efs/common.h"
#include "efs/metadata.h"
#include <stdint.h>

/* Length-prefixed TCP frames: 4 bytes length (network order), 1 byte type,
 * length-1 bytes payload. */

enum efs_msg_type {
    EFS_MSG_HEARTBEAT = 0,
    EFS_MSG_HEARTBEAT_ACK = 1,
    EFS_MSG_HELLO = 2,
    EFS_MSG_HELLO_ACK = 3,
    EFS_MSG_GET_CHUNK = 4,
    EFS_MSG_GET_CHUNK_REPLY = 5,
    EFS_MSG_PUT_CHUNK = 6,
    EFS_MSG_PUT_CHUNK_REPLY = 7,
    EFS_MSG_GET_META = 8,
    EFS_MSG_GET_META_REPLY = 9,
    EFS_MSG_PUT_META = 10,
    EFS_MSG_PUT_META_REPLY = 11,
    EFS_MSG_LIST_NODES = 12,
    EFS_MSG_LIST_NODES_REPLY = 13,
    EFS_MSG_CREATE_EXPORT = 14,
    EFS_MSG_CREATE_EXPORT_REPLY = 15,
    EFS_MSG_JOIN = 16,
    EFS_MSG_JOIN_REPLY = 17,
    EFS_MSG_STATUS = 18,
    EFS_MSG_STATUS_REPLY = 19,
    EFS_MSG_REMOVE_NODE = 20,
    EFS_MSG_REMOVE_NODE_REPLY = 21,
    EFS_MSG_SHRINK_QUOTA = 22,
    EFS_MSG_SHRINK_QUOTA_REPLY = 23,
    EFS_MSG_NODE_LEFT = 24,
    EFS_MSG_QUERY_STATS = 25,
    EFS_MSG_QUERY_STATS_REPLY = 26,
    EFS_MSG_LIST_EXPORTS = 27,
    EFS_MSG_LIST_EXPORTS_REPLY = 28,
    EFS_MSG_DRAIN_NODE = 29,
    EFS_MSG_DRAIN_NODE_REPLY = 30,
    EFS_MSG_UNDRAIN_NODE = 31,
    EFS_MSG_UNDRAIN_NODE_REPLY = 32,
    /* Network bench: same payload as PUT_CHUNK; server ACKs and discards. */
    EFS_MSG_BENCH_PUT = 33,
    EFS_MSG_BENCH_PUT_REPLY = 34,
    EFS_MSG_DESTROY_EXPORT = 35,
    EFS_MSG_DESTROY_EXPORT_REPLY = 36,
};

struct efs_msg_hello {
    uint32_t version;
    efs_node_id_t node_id;
    char addr[64];
    uint16_t port;
    char storage_path[EFS_MAX_PATH];
    uint64_t quota; /* local storage quota in bytes; 0 = unlimited */
    uint64_t used;  /* bytes currently stored on this node */
};

struct efs_msg_hello_ack {
    uint32_t epoch;
    efs_node_id_t assigned_id;
    uint32_t node_count;
    struct efs_node nodes[EFS_MAX_NODES];
};

struct efs_msg_get_chunk {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t chunk_index;
    uint32_t fragment_index; /* 0 = D1, 1 = D2, 2 = P */
};

/* Reply payload: [status:1][checksum:32][fragment_data:data_len]
 * data_len = frame_payload_len - 1 - EFS_HASH_SIZE when status == OK. */
#define EFS_GET_CHUNK_OK     0
#define EFS_GET_CHUNK_NOT_FOUND 1
#define EFS_GET_CHUNK_ERROR  2

/* PUT/BENCH frame: this header followed by data_len payload bytes. */
struct efs_msg_put_chunk {
    efs_export_id_t export_id;
    efs_ino_t ino;
    uint32_t chunk_index;
    uint32_t fragment_index;
    uint8_t checksum[EFS_HASH_SIZE];
    uint32_t data_len;
    /* uint8_t data[data_len]; */
};

#define EFS_PUT_CHUNK_OK     0
#define EFS_PUT_CHUNK_ERROR  1
#define EFS_PUT_CHUNK_QUOTA_EXCEEDED 2

#define EFS_BENCH_PUT_OK     0
#define EFS_BENCH_PUT_ERROR  1

struct efs_msg_put_meta {
    uint32_t epoch;
    efs_export_id_t export_id;
    /* serialized metadata follows as a length-prefixed blob */
};

#define EFS_PUT_META_OK     0
#define EFS_PUT_META_ERROR  1
#define EFS_PUT_META_STALE  2

struct efs_msg_put_meta_reply {
    uint8_t status;
    uint32_t new_epoch;
};

struct efs_msg_list_nodes_reply {
    uint32_t node_count;
    struct efs_node nodes[EFS_MAX_NODES];
};

struct efs_msg_status_reply {
    uint64_t quota;
    uint64_t used;
    uint32_t state; /* efsd_server_state: 0=active, 3=draining, 4=drained, ... */
};

struct efs_msg_shrink_quota {
    uint64_t amount; /* bytes to subtract from the current quota */
};

struct efs_msg_node_left {
    efs_node_id_t node_id;
};

#define EFS_REMOVE_NODE_OK     0
#define EFS_REMOVE_NODE_ERROR  1
#define EFS_REMOVE_NODE_IN_PROGRESS 2
#define EFS_REMOVE_NODE_NOT_DRAINED 3

#define EFS_DRAIN_NODE_OK           0
#define EFS_DRAIN_NODE_ERROR        1
#define EFS_DRAIN_NODE_IN_PROGRESS  2

#define EFS_UNDRAIN_NODE_OK     0
#define EFS_UNDRAIN_NODE_ERROR  1

#define EFS_SHRINK_QUOTA_OK     0
#define EFS_SHRINK_QUOTA_ERROR  1
#define EFS_SHRINK_QUOTA_IN_PROGRESS 2

/* Wire values match enum efsd_server_state in server_internal.h */
#define EFS_NODE_STATE_ACTIVE    0
#define EFS_NODE_STATE_LEAVING   1
#define EFS_NODE_STATE_SHRINKING 2
#define EFS_NODE_STATE_DRAINING  3
#define EFS_NODE_STATE_DRAINED   4

struct efs_msg_create_export {
    char name[EFS_MAX_NAME];
    uint32_t chunk_size; /* 0 = EFS_DEFAULT_CHUNK_SIZE */
};

#define EFS_CREATE_EXPORT_OK                0
#define EFS_CREATE_EXPORT_ERROR             1
#define EFS_CREATE_EXPORT_EXISTS            2
/* Export was created/saved locally; peer meta replicate did not get quorum. */
#define EFS_CREATE_EXPORT_REPLICATE_FAILED  3

struct efs_msg_destroy_export {
    char name[EFS_MAX_NAME];
};

#define EFS_DESTROY_EXPORT_OK         0
#define EFS_DESTROY_EXPORT_ERROR      1
#define EFS_DESTROY_EXPORT_NOT_FOUND  2

struct efs_msg_join {
    char peer_host[64];
    uint16_t peer_port;
};

#define EFS_JOIN_OK     0
#define EFS_JOIN_ERROR  1

#define EFS_MAX_QUERY_USERS 256

struct efs_user_stat {
    uint32_t uid;
    uint64_t files;
    uint64_t bytes;
};

struct efs_msg_query_stats_reply {
    uint64_t total_files;
    uint64_t total_bytes;
    uint32_t user_count;
    struct efs_user_stat users[EFS_MAX_QUERY_USERS];
};

struct efs_msg_export_entry {
    efs_export_id_t id;
    char name[EFS_MAX_NAME];
};

struct efs_msg_list_exports_reply {
    uint32_t export_count;
    struct efs_msg_export_entry exports[EFS_MAX_EXPORTS];
};

/* Send a single message. */
int efs_send_msg(int fd, uint8_t type, const void *payload, uint32_t payload_len);

/* Send a message whose payload is two contiguous logical parts (writev),
 * avoiding a bounce buffer when the on-wire payload is header+body. */
int efs_send_msg_parts(int fd, uint8_t type,
                       const void *part1, uint32_t part1_len,
                       const void *part2, uint32_t part2_len);

/* Receive a single message. Caller must free *payload with free(). */
int efs_recv_msg(int fd, uint8_t *type, void **payload, uint32_t *payload_len);

/* Zero-copy variant: receive a message whose payload is exactly
 * [1-byte status][hdr_len bytes header][body_len bytes body]. Status, header,
 * and body are read directly into caller-provided buffers (no malloc, no extra
 * copy). The wire payload length must equal 1 + hdr_len + body_len exactly,
 * else EFS_ERR_PROTO. On success *type and *status are set. */
int efs_recv_msg_into(int fd, uint8_t *type, uint8_t *status,
                      void *hdr, uint32_t hdr_len, void *body, uint32_t body_len);

/* Hot-path helper: receive a 1-byte status reply without malloc.
 * On success sets *type and *status. Returns EFS_OK or an error. */
int efs_recv_u8_reply(int fd, uint8_t *type, uint8_t *status);

#endif
