#ifndef EFS_OPID_H
#define EFS_OPID_H

#include "efs/common.h"

/* Request identity + bounded dedup window (architecture.md §7.9 / I16).
 * Production RPCs do not carry this yet — the layout lives here so the
 * simulator can prove replay-safety before a wire bump. Do not invent
 * fields: (client UUID, session epoch, request sequence). */

#define EFS_OPID_UUID_LEN    16
#define EFS_OPID_BITMAP_BITS 64
#define EFS_OPID_REPLY_CACHE 16

struct efs_opid {
    uint8_t client_uuid[EFS_OPID_UUID_LEN];
    uint32_t session_epoch;
    uint64_t seq; /* 1-based; 0 is invalid */
};

struct efs_opid_reply {
    uint64_t seq;
    int rc;
    efs_ino_t ino;
    uint64_t extra; /* reserved: O_APPEND offset, etc. */
};

struct efs_opid_window {
    uint8_t client_uuid[EFS_OPID_UUID_LEN];
    uint32_t session_epoch;
    int inited;
    uint64_t highest_contiguous_seq; /* 0 = none completed */
    uint64_t bitmap; /* bit i => seq (highest + 1 + i) completed */
    struct efs_opid_reply cache[EFS_OPID_REPLY_CACHE];
    int ncache;
};

void efs_opid_window_init(struct efs_opid_window *w,
                          const uint8_t uuid[EFS_OPID_UUID_LEN],
                          uint32_t epoch);

/* 1 = already completed (fills *out from cache, or a stub if reclaimed).
 * 0 = new. Negative = error (epoch/uuid mismatch, seq 0). */
int efs_opid_lookup(const struct efs_opid_window *w, const struct efs_opid *id,
                    struct efs_opid_reply *out);

int efs_opid_complete(struct efs_opid_window *w, const struct efs_opid *id,
                      const struct efs_opid_reply *reply);

/* Client highest *contiguous* acknowledged seq. Records at or below may
 * be released. Never driven by a timer. */
int efs_opid_ack(struct efs_opid_window *w, uint64_t contiguous_ack);

#endif
