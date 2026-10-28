#ifndef EFS_REQUEST_RECEIPT_H
#define EFS_REQUEST_RECEIPT_H

#include "efs/common.h"
#include <pthread.h>

/* W155: per-request receipts for REPORT_CHUNKS on the host that served it.
 *
 * Key: the mount session (client UUID, epoch) plus the REPORT's W77
 * publication id (mint_ns, seq). A re-send of one built REPORT reuses the
 * id, so every attempt of one request lands on one receipt. The receipt
 * records how the attempts that ran here ended, so a client whose reply was
 * lost can ask instead of guessing, and a duplicate that arrives while the
 * first attempt still runs joins it instead of running beside it.
 *
 * In memory and bounded on purpose: a receipt in Raft would cost an entry
 * per REPORT (K32 budget). A missing receipt (evicted, restarted host, other
 * node) is never "not applied"; the client re-sends the same identity,
 * which the REPORT path already treats as idempotent (committed records
 * skip, W77 units join their deterministic transaction). */

#define EFS_RECEIPT_STATE_ABSENT  0 /* no receipt on this host */
#define EFS_RECEIPT_STATE_PENDING 1 /* an attempt is running here */
#define EFS_RECEIPT_STATE_DONE    2 /* the attempts here have ended */

/* Merged over every attempt that ran here; a later attempt never lowers
 * what an earlier one proved (APPLIED > PARTIAL > UNKNOWN > NONE). */
#define EFS_RECEIPT_OUT_NONE    0 /* nothing was proposed: refused or bounced */
#define EFS_RECEIPT_OUT_UNKNOWN 1 /* proposed, verdict not observed (or forwarded) */
#define EFS_RECEIPT_OUT_PARTIAL 2 /* STALE: all but the CAS losers committed */
#define EFS_RECEIPT_OUT_APPLIED 3 /* the whole request committed */

/* efs_receipt_run results (how a request was answered). */
#define EFS_RECEIPT_RAN     0 /* executed here */
#define EFS_RECEIPT_REPLAY  1 /* answered from an APPLIED receipt, not run */
#define EFS_RECEIPT_JOINED  2 /* waited for a running attempt and took its rc */
#define EFS_RECEIPT_WAITING 3 /* the running attempt outlived the join wait: BUSY */
#define EFS_RECEIPT_REFUSED 4 /* probe window full of pending owners: BUSY, not run */

struct efs_receipt_key {
    uint8_t uuid[16];
    uint32_t epoch;
    uint64_t mint_ns, seq;
};

/* K38: the first record an attempt lost on its own CAS base (a precheck
 * STALE), so a PARTIAL verdict names what stayed behind. ino 0: none was
 * recorded (no loser, or a loser found only at apply). */
struct efs_receipt_stale {
    efs_ino_t ino;
    uint64_t want, have; /* the record's base, the committed generation */
    uint32_t ci, span_off, span_len;
    char why[12];
};

struct efs_receipt {
    struct efs_receipt_key key;
    uint32_t state, outcome;
    int32_t rc;          /* last attempt's rc */
    uint32_t attempts;   /* attempts executed here */
    uint32_t joined;     /* duplicates that joined a running attempt */
    uint32_t replayed;   /* duplicates answered from an APPLIED receipt */
    efs_ino_t ino;       /* first record's inode */
    uint32_t nrec;
    uint64_t begin_ms, end_ms; /* monotonic, last attempt */
    char phase[16];      /* failing stage of the last attempt, "-" if none */
    struct efs_receipt_stale stale; /* last STALE attempt's first loser */
};

struct efs_receipt_table {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t mask;       /* capacity - 1, power of two */
    struct efs_receipt *slot;
    uint64_t stores, evictions;
};

/* cap is rounded up to a power of two, at least 16. */
int efs_receipt_table_init(struct efs_receipt_table *t, uint32_t cap);
void efs_receipt_table_free(struct efs_receipt_table *t);

uint32_t efs_receipt_outcome(int rc, int proposed);
uint32_t efs_receipt_merge(uint32_t prior, uint32_t next);

/* 1 and *out filled when this host holds a receipt for k, else 0. */
int efs_receipt_lookup(struct efs_receipt_table *t, const struct efs_receipt_key *k,
                       struct efs_receipt *out);

/* The attempt this host executes for k. exec returns the reply rc and sets
 * *proposed when anything may have been proposed, *phase to its failing
 * stage (or NULL), *stale to its first CAS loser (zeroed on entry). A duplicate of a request whose receipt is APPLIED is
 * answered OK without running. A duplicate of a running attempt waits up to
 * join_ms for it: APPLIED/PARTIAL take that rc; any other end runs again
 * (same identity, idempotent); still running answers EFS_ERR_BUSY.
 * Pending owners are never evicted. A full pending probe window refuses new
 * identities with BUSY/REFUSED without running exec or creating a receipt.
 * *how is one of EFS_RECEIPT_RAN..REFUSED; *out the receipt afterwards. */
typedef int (*efs_receipt_exec_fn)(void *ctx, int *proposed, const char **phase,
                                   struct efs_receipt_stale *stale);
int efs_receipt_run(struct efs_receipt_table *t, const struct efs_receipt_key *k,
                    efs_ino_t ino, uint32_t nrec, uint32_t join_ms,
                    efs_receipt_exec_fn exec, void *ctx, int *how,
                    struct efs_receipt *out);

/* "uuid8/epoch/mint:seq", for log lines on both sides. */
void efs_receipt_key_format(const struct efs_receipt_key *k, char *buf, size_t cap);
const char *efs_receipt_outcome_name(uint32_t outcome);

#endif
