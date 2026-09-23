#ifndef EFS_OPID_H
#define EFS_OPID_H

#include "efs/common.h"

/* Request identity + bounded dedup window (architecture.md §7.9 / I16).
 * Do not invent fields: (client UUID, session epoch, request sequence).
 *
 * Production directory RPCs (CREATE/MKDIR, UNLINK/RMDIR, LINK, RENAME_AT)
 * carry an optional EFS_OPID_WIRE_LEN suffix: the identity plus the
 * client's highest *contiguous* acknowledged seq (§7.9 reclamation). The
 * window for a directory op lives on the shard that served it — the
 * dentry shard of the (first) named entry — under efs_kv_key_opid(shard,
 * uuid, epoch). The seq space is per client, not per shard, so a shard's
 * window sees a sparse subset; the ack watermark is what lets it advance
 * past seqs it never served. */

#define EFS_OPID_UUID_LEN    16
#define EFS_OPID_BITMAP_BITS 64
#define EFS_OPID_REPLY_CACHE 16

struct efs_opid {
    uint8_t client_uuid[EFS_OPID_UUID_LEN];
    uint32_t session_epoch;
    uint64_t seq; /* 1-based; 0 is invalid */
};

/* What a mutating RPC carries: the identity plus the client's contiguous
 * ack. ack < seq always (the op itself is not acked while in flight). */
struct efs_opid_req {
    struct efs_opid id;
    uint64_t ack;
};

/* Wire suffix: uuid[16] + native u32 epoch + native u64 seq + native u64
 * ack (same convention as the EFS_SESS_WIRE_LEN suffix). */
#define EFS_OPID_WIRE_LEN 36u
void efs_opid_req_pack(const struct efs_opid_req *q, uint8_t out[EFS_OPID_WIRE_LEN]);
void efs_opid_req_unpack(struct efs_opid_req *q, const uint8_t in[EFS_OPID_WIRE_LEN]);
/* 1 when q names a real identity (non-zero uuid, seq != 0). */
int efs_opid_req_valid(const struct efs_opid_req *q);

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

/* Client highest *contiguous* acknowledged seq. Everything at or below is
 * done from the client's point of view (it will never retry those), so
 * the watermark advances to it — including past seqs this shard never
 * served — and cached replies at or below are released. A lower ack than
 * the current watermark is a stale retry's value and is ignored. Never
 * driven by a timer. */
int efs_opid_ack(struct efs_opid_window *w, uint64_t contiguous_ack);

#define EFS_OPID_VAL_MAX 512
int efs_opid_window_pack(const struct efs_opid_window *w, uint8_t *out, uint32_t *len);
int efs_opid_window_unpack(struct efs_opid_window *w, const uint8_t *in, uint32_t len);

/* Commutative fold used by both the log path and the txn REDUCE resolve:
 * take the window as stored (`cur`, or empty when cn == 0), apply the ack,
 * record `reply` for q->id, and pack the result into `out`. A window that
 * cannot hold the record (seq more than EFS_OPID_BITMAP_BITS above the
 * watermark, or a full reply cache) is left as it was and EFS_ERR_NOMEM
 * is returned — the op still applies, it is just not replay-protected.
 * `uuid`/`epoch` seed an empty window. */
int efs_opid_fold(const uint8_t *cur, uint32_t cn,
                  const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch,
                  const struct efs_opid_req *q, const struct efs_opid_reply *reply,
                  uint8_t *out, uint32_t *olen);

/* Window lookup from the stored bytes (cn == 0 = no window yet). Same
 * return convention as efs_opid_lookup; a uuid/epoch mismatch on the
 * stored window is a corrupted key, EFS_ERR_PROTO. */
int efs_opid_probe(const uint8_t *cur, uint32_t cn, const struct efs_opid *id,
                   struct efs_opid_reply *out);

#endif
