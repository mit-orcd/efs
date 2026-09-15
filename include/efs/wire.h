#ifndef EFS_WIRE_H
#define EFS_WIRE_H

#include "efs/protocol.h"
#include <stdint.h>

/* Pure encode/decode for the length-prefixed frame and the C-struct
 * payloads. No sockets, no efs_conn, no RDMA.
 *
 * On-wire frame: [u32be nlen][u8 type][nlen-1 bytes payload]
 * nlen == 1 + payload_len; rejected if nlen == 0 or nlen > EFS_MSG_MAX_LEN.
 *
 * Message payloads are native-endian C layout (identity memcpy). That is
 * the current encoding; this module does not invent a second serializer. */

/* Total bytes of a frame with this payload: 4 + 1 + payload_len. */
int efs_wire_frame_size(uint32_t payload_len, uint32_t *frame_len);

/* Reject nlen == 0 or nlen > EFS_MSG_MAX_LEN. nlen is the on-wire length
 * field (1 + payload). */
int efs_wire_frame_check_nlen(uint32_t nlen);

/* Write the 5-byte prefix into out. TCP send uses this with writev so a
 * multi-gigabyte GET_META payload is never bounced through a frame buffer. */
int efs_wire_frame_header(uint8_t type, uint32_t payload_len, uint8_t out[5]);

/* Encode type + two payload parts into out. out_cap must cover the whole
 * frame (see efs_wire_frame_size). */
int efs_wire_frame_encode(uint8_t type, const void *p1, uint32_t n1,
                          const void *p2, uint32_t n2,
                          uint8_t *out, uint32_t out_cap, uint32_t *out_len);

/* Parse a complete frame in [in, in+in_len). On success *payload points
 * into `in` (not a copy) and is valid for *plen bytes. */
int efs_wire_frame_decode(const uint8_t *in, uint32_t in_len,
                          uint8_t *type, const uint8_t **payload, uint32_t *plen);

/* Identity pack/unpack: memcpy + length check. The current wire IS native
 * C layout. Variable-length messages pack the header struct; trailing
 * bytes stay a second payload part. */
int efs_wire_pack(const void *msg, uint32_t len, void *out, uint32_t out_cap);
int efs_wire_unpack(const void *in, uint32_t in_len, void *msg, uint32_t len);

/* efs_raft_msg codec (the one non-C-layout encoding: the message has a
 * variable-length entry payload). All fixed fields big-endian:
 *   u8 type, u8 group, u8 vote_granted, u8 success,
 *   s32 from, s32 to,
 *   u64 term, u64 boot_id, u64 last_log_index, u64 last_log_term,
 *   u64 prev_index, u64 prev_term, u64 leader_commit, u64 match_index,
 *   u32 nentries (0..EFS_RAFT_AE_MAX — a catch-up AE may carry a batch),
 *   per entry: u64 term, u32 clen, u8 cmd[clen].
 * decode packs every cmd into cmd_buf (caller-owned) and points
 * msg->entries[i].cmd at the matching offset. */
#define EFS_WIRE_RAFT_HDR_LEN 80u /* fixed part through nentries */
#define EFS_WIRE_RAFT_MAX_CMD (4u * 1024 * 1024)

struct efs_raft_msg; /* efs/raft.h */
int efs_wire_raft_encode(const struct efs_raft_msg *msg, uint8_t *out,
                         uint32_t out_cap, uint32_t *out_len);
int efs_wire_raft_decode(const uint8_t *in, uint32_t in_len,
                         struct efs_raft_msg *msg, uint8_t *cmd_buf,
                         uint32_t cmd_cap);

#endif
