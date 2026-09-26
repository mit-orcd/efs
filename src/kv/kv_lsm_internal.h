#ifndef EFS_KV_LSM_INTERNAL_H
#define EFS_KV_LSM_INTERNAL_H

#include "efs/checksum.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Shared by kv_lsm.c / kv_wal.c / kv_seg.c. Not a public header.
 *
 * WAL record:  u32 crc32(payload) · u32 paylen · payload
 *   payload:   u32 nitems · nitems × (u8 op · u32 klen · u32 vlen · key · val)
 * A short or crc-failing record ends the replay: it is the crash point.
 *
 * Segment file: block* · index · footer(32B, at EOF)
 *   block:     entry* , entries never split, cut at KV_LSM_BLOCK_TARGET
 *   entry:     u8 op · u32 klen · u32 vlen · key · val
 *   index:     nblocks × (u32 klen · key · u64 off · u32 len)  -- first key
 *   footer:    u64 index_off · u32 index_len · u32 nblocks · u32 magic
 *              u32 version · u32 crc32(first 24B) · u32 pad
 * Only the index is resident, so RAM per segment is one key per block. */

#define KV_LSM_BLOCK_TARGET (64u * 1024u)
#define KV_LSM_MEM_DEFAULT  (4u * 1024u * 1024u)
#define KV_LSM_L0_DEFAULT   4u
#define KV_LSM_MAX_SEGS     64u
#define KV_LSM_KLEN_MAX     (64u * 1024u)
#define KV_LSM_VLEN_MAX     (16u * 1024u * 1024u)
/* Enforced at append AND at replay: a record replay would refuse to read
 * must be impossible to commit, or a crash silently drops ACKed writes. */
#define KV_LSM_REC_MAX      (256u * 1024u * 1024u)
#define KV_LSM_FOOTER_LEN   32u
#define KV_LSM_SEG_MAGIC    0x4B565347u /* KVSG */
#define KV_LSM_SEG_VERSION  1u

#define KV_OP_PUT 1
#define KV_OP_DEL 2 /* tombstone: shadows older segments, never returned */

static inline int kv_key_cmp(const uint8_t *a, uint32_t al, const uint8_t *b,
                             uint32_t bl)
{
    uint32_t n = al < bl ? al : bl;
    int c = n ? memcmp(a, b, n) : 0;

    if (c != 0)
        return c;
    if (al < bl)
        return -1;
    if (al > bl)
        return 1;
    return 0;
}

static inline int kv_has_prefix(const uint8_t *k, uint32_t kl,
                                const uint8_t *p, uint32_t pl)
{
    return pl == 0 || (kl >= pl && memcmp(k, p, pl) == 0);
}


/* Growable scratch buffer for values read out of a segment. */
struct kv_buf {
    uint8_t *p;
    uint32_t len;
    uint32_t cap;
};

int kv_buf_set(struct kv_buf *b, const uint8_t *src, uint32_t n);
void kv_buf_free(struct kv_buf *b);

/* --- WAL ------------------------------------------------------------- */

struct kv_wal;

int kv_wal_open(const char *path, int sync_mode, struct kv_wal **out);
void kv_wal_close(struct kv_wal *w);
/* Appends one record and returns only once it is durable (SYNC mode).
 * Concurrent callers share one fsync. *out_seq is the record's position in
 * the log, or 0 when nothing was written; it is set even when a later fsync
 * fails, so the caller can still release its place in the apply order. */
int kv_wal_append(struct kv_wal *w, const struct efs_kv_item *items, uint32_t n,
                  uint64_t *out_seq);
/* on=1 defers the next appends' fsync; on=0 (last nest) fsyncs them. */
int kv_wal_hold(struct kv_wal *w, int on);
/* Empties the log; call only after the memtable is a durable segment. */
int kv_wal_reset(struct kv_wal *w);
int kv_wal_replay(const char *path,
                  int (*cb)(void *user, uint8_t op, const uint8_t *key,
                            uint32_t klen, const uint8_t *val, uint32_t vlen),
                  void *user);

/* --- engine (shared by kv_lsm.c and kv_compact.c) -------------------- */

#define KV_LSM_L1_TARGET (64ull * 1024ull * 1024ull)
#define KV_LSM_PATH_MAX  1024

struct kv_ent {
    uint8_t op;
    uint32_t klen;
    uint32_t vlen;
    uint8_t *key;
    uint8_t *val;
};

/* Sorted by key; binary search to find, memmove to insert. */
struct kv_mtab {
    struct kv_ent **e;
    uint32_t n;
    uint32_t cap;
    uint64_t bytes;
};

struct seg_slot {
    struct kv_seg *seg;
    uint64_t seq;
    int level;
};

struct kv_lsm {
    uint32_t magic;
    char dir[KV_LSM_PATH_MAX];
    struct efs_kv_lsm_cfg cfg;
    struct kv_wal *wal;
    struct kv_mtab mt;
    struct seg_slot l0[KV_LSM_MAX_SEGS]; /* newest first */
    uint32_t n_l0;
    struct seg_slot l1[KV_LSM_MAX_SEGS]; /* ascending, disjoint ranges */
    uint32_t n_l1;
    uint64_t next_seq;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint64_t apply_next; /* WAL seq whose turn it is to hit the memtable */
    int io_failed;
    int fail_next_batch;
    struct kv_buf scratch;
};

/* One merge input: the memtable when it holds no iterator, else a segment.
 * Precedence is the array position, newest first. */
struct msrc {
    struct kv_seg_iter *it;
    uint32_t mi;
    const uint8_t *key;
    uint32_t klen;
    const uint8_t *val;
    uint32_t vlen;
    uint8_t op;
    int done;
};

void kv_seg_path(const struct kv_lsm *l, int level, uint64_t seq, char *out,
                 size_t n);
void kv_sync_dir(const char *dir);
void kv_mtab_clear(struct kv_mtab *m);
uint32_t kv_mtab_pos(const struct kv_mtab *m, const uint8_t *key, uint32_t klen,
                     int *found);
int kv_mtab_set(struct kv_mtab *m, uint8_t op, const uint8_t *key,
                uint32_t klen, const uint8_t *val, uint32_t vlen);
/* Advances past keys below lower; sets done at the end of the source. */
int kv_msrc_advance(struct kv_lsm *l, struct msrc *s, const uint8_t *lower,
                    uint32_t lower_len);
int kv_manifest_write(struct kv_lsm *l);
int kv_l1_cmp(const void *a, const void *b);

/* Callers hold l->mu. */
int kv_flush_locked(struct kv_lsm *l);
int kv_compact_locked(struct kv_lsm *l);
int kv_maybe_flush_locked(struct kv_lsm *l);

/* --- segments -------------------------------------------------------- */

struct kv_seg;
struct kv_seg_w;
struct kv_seg_iter;

int kv_seg_open(const char *path, struct kv_seg **out);
void kv_seg_close(struct kv_seg *s);
/* EFS_OK with *op set, or EFS_ERR_NOT_FOUND when this segment is silent
 * about the key. A tombstone is EFS_OK with *op == KV_OP_DEL. */
int kv_seg_get(struct kv_seg *s, const uint8_t *key, uint32_t klen,
               struct kv_buf *val, uint8_t *op);
/* Where a miss has to read. fd/off/len are copied so the pread can run
 * without the LSM lock; the segment stays alive across that only if the
 * caller pinned it first. */
struct kv_seg_io {
    int fd;
    uint64_t off;
    uint32_t len;
    uint32_t bi;
};
/* EFS_OK and *need_io == 0: found (*op set). EFS_ERR_NOT_FOUND: this
 * segment does not hold the key. EFS_OK and *need_io == 1: *io is filled. */
int kv_seg_probe(struct kv_seg *s, const uint8_t *key, uint32_t klen,
                 struct kv_buf *val, uint8_t *op, int *need_io,
                 struct kv_seg_io *io);
int kv_seg_read(const struct kv_seg_io *io, uint8_t **blk);
/* Takes ownership of blk. Caches it, then searches. */
int kv_seg_install(struct kv_seg *s, const struct kv_seg_io *io, uint8_t *blk,
                   const uint8_t *key, uint32_t klen, struct kv_buf *val,
                   uint8_t *op);
void kv_seg_pin(struct kv_seg *s);
void kv_seg_unpin(struct kv_seg *s);
int kv_seg_first_key(struct kv_seg *s, const uint8_t **key, uint32_t *klen);
int kv_seg_last_key(struct kv_seg *s, const uint8_t **key, uint32_t *klen);

int kv_seg_iter_open(struct kv_seg *s, struct kv_seg_iter **out);
void kv_seg_iter_close(struct kv_seg_iter *it);
/* Positions at the first key >= seek (whole segment when plen is 0). */
int kv_seg_iter_seek(struct kv_seg_iter *it, const uint8_t *seek, uint32_t plen);
/* EFS_OK and pointers valid until the next call, or EFS_ERR_NOT_FOUND at end. */
int kv_seg_iter_next(struct kv_seg_iter *it, const uint8_t **key, uint32_t *klen,
                     const uint8_t **val, uint32_t *vlen, uint8_t *op);

int kv_seg_w_open(const char *path, struct kv_seg_w **out);
/* Keys must arrive strictly ascending. */
int kv_seg_w_add(struct kv_seg_w *w, uint8_t op, const uint8_t *key,
                 uint32_t klen, const uint8_t *val, uint32_t vlen);
uint64_t kv_seg_w_bytes(const struct kv_seg_w *w);
int kv_seg_w_finish(struct kv_seg_w *w);
void kv_seg_w_abort(struct kv_seg_w *w);

#endif
