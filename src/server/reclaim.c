/* W124 node reclaim: batched, journal-first acceptance of dead inodes.
 *
 * A leader sends one EFS_MSG_GC_INODE_BATCH per node per batch. Acceptance:
 *   1. append the items to the current journal segment and fsync it (the
 *      commit point; a new segment first advances the durable state record);
 *   2. install each inode's fence under its write guard, so a PUT that held
 *      the guard finishes first and every later PUT is refused (the PUT paths
 *      check inode_is_dead, which consults this fence set, under the guard);
 *   3. credit the inode's bytes back to quota (they become reclaim-pending);
 *   4. reply OK. The node now owns the remaining physical work.
 * Background threads then checkpoint the death-bitmap pages (one update per
 * page), delete the fragment directories (deleter pool), and retire whole
 * journal segments once every record in them is checkpointed and deleted.
 *
 * Journal layout:
 *   storage_paths[0]/.efs-reclaim-state  epoch, oldest and next segment
 *       sequence (checksummed), outside meta/ on purpose;
 *   storage_paths[0]/meta/gc-journal/<seq:016llx>.seg  64-byte header +
 *       records, each checksummed: version 1 (W124) 64-byte inode records,
 *       version 2 (W125) 128-byte records with a kind: a dead inode, a
 *       retired packed segment (its fence keys EFS_RECLAIM_SEGMENT_EID) or
 *       one superseded fragment of a live inode.
 *       New segments are version 2; version 1 segments still replay.
 * Segments [oldest, next) must all exist. Retirement advances `oldest` in the
 * state record before deleting the segment file, so a crash in between leaves
 * only a leftover file below `oldest`, which startup removes. Completion is not
 * persisted: replay re-runs checkpoint and deletion, which are idempotent.
 *
 * Any verification failure at startup (missing or corrupt segment, sequence
 * gap, state older than the segments, a journal directory without a state
 * record) fails closed: data PUTs are refused (inode_is_dead returns -1) and
 * a recovery thread moves the journal aside, starts a new epoch, scans every
 * local inode directory, and fences each inode that metadata proves dead
 * (row absent with durable history) before writes are re-admitted. */
#include "server_internal.h"
#include "efs/checksum.h"
#include "efs/pack_mapping.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define RJ_HDR_MAGIC "EFSRJNL1"
#define RJ_STATE_MAGIC "EFSRJST1"
#define RJ_VERSION 2u
#define RJ_HDR_BYTES 64u
#define RJ_REC_V1_BYTES 64u
#define RJ_REC_BYTES 128u
#define RJ_REC_DATA 96u
#define RJ_REC2_TAG 0x52524532u /* "RRE2" */
#define RJ_STATE_BYTES 72u
/* The state record lives in the storage root, outside meta/, so restoring
 * an older (self-consistent) copy of meta/ shows up as missing segments. */
#define RJ_STATE_FILE ".efs-reclaim-state"
#define RJ_REC_TAG 0x52524543u /* "RREC" */
#define RJ_SEG_RECORDS 16384u
#define RJ_CAP (1u << 18)       /* unretired records per node */
#define RJ_DELETERS 4
#define RJ_DELETE_BATCH 64u
#define RJ_CHECKPOINT_BATCH RJ_CAP /* two syncfs per checkpoint: take everything pending */
#define RJ_JOURNAL_MIN (256ull << 20) /* physical floor kept for journal/checkpoints */

struct rj_rec {
    uint8_t kind; /* EFS_RECLAIM_INODE, _SEGMENT or _SUPERSEDED */
    uint8_t fi;   /* SUPERSEDED: fragment index */
    uint32_t ci;  /* SUPERSEDED: chunk index */
    uint8_t sum[EFS_HASH_SIZE]; /* SUPERSEDED: the fragment's checksum */
    uint32_t eid;
    efs_ino_t ino;
    uint64_t gen;
    uint64_t seg;
    uint64_t credited, removed; /* quota moved to pending; payload unlinked so far */
    uint8_t checkpointed, deleted, claimed;
};

struct rj_seg {
    uint64_t seq, first, count; /* record indices [first, first+count) */
};

static struct {
    pthread_mutex_t mu;
    pthread_cond_t work, idle;
    struct efsd_server *s;
    char dir[4096], root[4096];
    int ready, stop, recovering, rec_started;
    int wedged; /* an append failed and its tail could not be cut: refuse until restart */
    int hold;   /* unit tests only: park the deleters */
    uint64_t epoch, oldest, next; /* durable state record */
    int seg_fd;                   /* open current segment, -1 none */
    uint64_t seg_seq, seg_count;  /* current segment and its records */
    /* Ring of unretired records: absolute indices [head, tail). */
    struct rj_rec *recs;
    uint64_t head, tail;
    struct rj_seg segs[RJ_CAP / RJ_SEG_RECORDS + 64];
    uint32_t nseg;
    uint64_t delete_cursor, checkpoint_cursor;
    pthread_t bg, del[RJ_DELETERS], rec_tid;
    int threads;
    uint64_t accepted, deleted, checkpointed, retired, busy, credited, recovered;
    uint64_t last_append_us;
} rj = {.mu = PTHREAD_MUTEX_INITIALIZER, .work = PTHREAD_COND_INITIALIZER,
        .idle = PTHREAD_COND_INITIALIZER, .seg_fd = -1};

/* ---- fence set: (eid, ino) of unretired records ---- */
static pthread_rwlock_t fence_mu = PTHREAD_RWLOCK_INITIALIZER;
static uint64_t *fence_keys; /* 0 = empty, 1 = tombstone, else (eid<<56)^ino|... */
static uint32_t *fence_eids;
static uint64_t fence_cap, fence_used, fence_live;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
    return x;
}
static int fence_find_locked(uint32_t eid, efs_ino_t ino, uint64_t *slot)
{
    if (!fence_cap) return 0;
    uint64_t i = mix64(ino ^ ((uint64_t)eid << 40)) & (fence_cap - 1);
    for (uint64_t probe = 0; probe < fence_cap; probe++, i = (i + 1) & (fence_cap - 1)) {
        if (fence_keys[i] == 0) return 0;
        if (fence_keys[i] == ino + 2 && fence_eids[i] == eid) { *slot = i; return 1; }
    }
    return 0;
}
/* Tombstones only end at a rebuild; keep probe chains short (live entries
 * are at most RJ_CAP of 2*RJ_CAP slots). */
static void fence_rebuild_locked(void)
{
    uint64_t *keys = calloc(fence_cap, sizeof(*keys));
    uint32_t *eids = calloc(fence_cap, sizeof(*eids));
    if (!keys || !eids) { free(keys); free(eids); return; }
    uint64_t used = 0;
    for (uint64_t k = 0; k < fence_cap; k++) {
        if (fence_keys[k] < 2) continue;
        uint64_t i = mix64((fence_keys[k] - 2) ^ ((uint64_t)fence_eids[k] << 40)) & (fence_cap - 1);
        while (keys[i]) i = (i + 1) & (fence_cap - 1);
        keys[i] = fence_keys[k];
        eids[i] = fence_eids[k];
        used++;
    }
    free(fence_keys);
    free(fence_eids);
    fence_keys = keys;
    fence_eids = eids;
    fence_used = used;
}
static int fence_insert_locked(uint32_t eid, efs_ino_t ino)
{
    uint64_t slot;
    if (fence_find_locked(eid, ino, &slot)) return 0;
    if (fence_used >= fence_cap - fence_cap / 4) fence_rebuild_locked();
    uint64_t i = mix64(ino ^ ((uint64_t)eid << 40)) & (fence_cap - 1);
    for (;; i = (i + 1) & (fence_cap - 1)) {
        if (fence_keys[i] == 0 || fence_keys[i] == 1) {
            if (fence_keys[i] == 0) fence_used++;
            fence_keys[i] = ino + 2;
            fence_eids[i] = eid;
            fence_live++;
            return 1;
        }
    }
}
static void fence_remove_locked(uint32_t eid, efs_ino_t ino)
{
    uint64_t slot;
    if (fence_find_locked(eid, ino, &slot)) {
        fence_keys[slot] = 1;
        fence_live--;
    }
}
int server_reclaim_fenced(efs_export_id_t eid, efs_ino_t ino)
{
    if (!__atomic_load_n(&fence_live, __ATOMIC_ACQUIRE)) return 0;
    uint64_t slot;
    pthread_rwlock_rdlock(&fence_mu);
    int hit = fence_find_locked(eid, ino, &slot);
    pthread_rwlock_unlock(&fence_mu);
    return hit;
}

/* ---- encoding ---- */
static void put64(uint8_t *p, uint64_t v) { for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
static void put32(uint8_t *p, uint32_t v) { for (int i = 3; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }
static uint32_t get32(const uint8_t *p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v = (v << 8) | p[i]; return v; }

static int write_all(int fd, const uint8_t *b, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, b + off, n - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return EFS_ERR_IO;
        off += (size_t)w;
    }
    return EFS_OK;
}
static int fsync_fd(int fd)
{
    int r;
    do { r = fsync(fd); } while (r < 0 && errno == EINTR);
    return r ? EFS_ERR_IO : EFS_OK;
}
static int fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return EFS_ERR_IO;
    int rc = fsync_fd(fd);
    close(fd);
    return rc;
}
static void seg_path(char *out, size_t n, uint64_t seq)
{
    snprintf(out, n, "%s/%016llx.seg", rj.dir, (unsigned long long)seq);
}

static int state_write(uint64_t epoch, uint64_t oldest, uint64_t next)
{
    uint8_t b[RJ_STATE_BYTES];
    memset(b, 0, sizeof(b));
    memcpy(b, RJ_STATE_MAGIC, 8);
    put32(b + 8, RJ_VERSION);
    put64(b + 16, epoch);
    put64(b + 24, oldest);
    put64(b + 32, next);
    efs_hash(b, 40, b + 40);
    char path[4200], tmp[4300];
    snprintf(path, sizeof(path), "%s/" RJ_STATE_FILE, rj.root);
    snprintf(tmp, sizeof(tmp), "%s/" RJ_STATE_FILE ".tmp", rj.root);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return EFS_ERR_IO;
    int rc = write_all(fd, b, sizeof(b));
    if (rc == EFS_OK) rc = fsync_fd(fd);
    if (close(fd)) rc = EFS_ERR_IO;
    if (rc == EFS_OK && rename(tmp, path)) rc = EFS_ERR_IO;
    if (rc == EFS_OK) rc = fsync_dir(rj.root);
    if (rc == EFS_OK) { rj.epoch = epoch; rj.oldest = oldest; rj.next = next; }
    return rc;
}
/* 1 = loaded, 0 = absent, <0 = corrupt or unreadable. */
static int state_read(void)
{
    char path[4200];
    snprintf(path, sizeof(path), "%s/" RJ_STATE_FILE, rj.root);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    uint8_t b[RJ_STATE_BYTES], h[EFS_HASH_SIZE];
    ssize_t n = pread(fd, b, sizeof(b), 0);
    close(fd);
    if (n != (ssize_t)sizeof(b) || memcmp(b, RJ_STATE_MAGIC, 8) || get32(b + 8) != RJ_VERSION)
        return -1;
    efs_hash(b, 40, h);
    if (memcmp(h, b + 40, EFS_HASH_SIZE)) return -1;
    rj.epoch = get64(b + 16);
    rj.oldest = get64(b + 24);
    rj.next = get64(b + 32);
    if (rj.oldest > rj.next) return -1;
    return 1;
}

/* Version 2: tag:4 kind:1 fragment:1 0:2 eid:4 chunk:4 ino:8 gen:8 seq:8
 * checksum:32 0:24, then hash:32 over the first 96 bytes. */
static void rec_encode(uint8_t *b, const struct rj_rec *r)
{
    memset(b, 0, RJ_REC_BYTES);
    put32(b, RJ_REC2_TAG);
    b[4] = r->kind;
    b[5] = r->fi;
    put32(b + 8, r->eid);
    put32(b + 12, r->ci);
    put64(b + 16, r->ino);
    put64(b + 24, r->gen);
    put64(b + 32, r->seg);
    memcpy(b + 40, r->sum, EFS_HASH_SIZE);
    efs_hash(b, RJ_REC_DATA, b + RJ_REC_DATA);
}
static int rec_valid(const struct rj_rec *r)
{
    if (r->kind == EFS_RECLAIM_SEGMENT)
        return (r->eid & 0x80000000u) && r->ino && r->gen;
    if (r->kind == EFS_RECLAIM_SUPERSEDED)
        return r->eid && !(r->eid & 0x80000000u) && r->ino > EFS_ROOT_INO && r->gen &&
               r->fi < EFS_NUM_FRAGMENTS;
    return r->kind == EFS_RECLAIM_INODE && r->eid && !(r->eid & 0x80000000u) &&
           r->ino > EFS_ROOT_INO;
}
static int rec_decode(const uint8_t *b, uint32_t version, uint64_t seq, struct rj_rec *r)
{
    uint8_t h[EFS_HASH_SIZE];
    memset(r, 0, sizeof(*r));
    if (version == 1) {
        efs_hash(b, 32, h);
        if (memcmp(h, b + 32, EFS_HASH_SIZE) || get32(b) != RJ_REC_TAG || get64(b + 24) != seq)
            return -1;
        r->kind = EFS_RECLAIM_INODE;
        r->eid = get32(b + 4);
        r->ino = get64(b + 8);
        r->gen = get64(b + 16);
    } else {
        efs_hash(b, RJ_REC_DATA, h);
        if (memcmp(h, b + RJ_REC_DATA, EFS_HASH_SIZE) || get32(b) != RJ_REC2_TAG || get64(b + 32) != seq)
            return -1;
        for (int i = 6; i < 8; i++) if (b[i]) return -1;
        for (unsigned i = 72; i < RJ_REC_DATA; i++) if (b[i]) return -1;
        r->kind = b[4];
        r->fi = b[5];
        r->eid = get32(b + 8);
        r->ci = get32(b + 12);
        r->ino = get64(b + 16);
        r->gen = get64(b + 24);
        memcpy(r->sum, b + 40, EFS_HASH_SIZE);
    }
    r->seg = seq;
    return rec_valid(r) ? 0 : -1;
}

/* Open a fresh segment: advance the durable state first. rj.mu held. */
static int seg_open_locked(void)
{
    uint64_t seq = rj.next;
    if (rj.nseg >= sizeof(rj.segs) / sizeof(rj.segs[0])) return EFS_ERR_BUSY;
    int rc;
    char path[4200];
    seg_path(path, sizeof(path), seq);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return EFS_ERR_IO;
    uint8_t h[RJ_HDR_BYTES];
    memset(h, 0, sizeof(h));
    memcpy(h, RJ_HDR_MAGIC, 8);
    put32(h + 8, RJ_VERSION);
    put32(h + 12, rj.s ? rj.s->id : 0);
    put64(h + 16, rj.epoch);
    put64(h + 24, seq);
    efs_hash(h, 32, h + 32);
    rc = write_all(fd, h, sizeof(h));
    if (rc == EFS_OK) rc = fsync_fd(fd);
    if (rc == EFS_OK) rc = fsync_dir(rj.dir);
    /* Only now advance the state: a crash before this leaves a header-only
     * segment at `next`, which startup recognises and removes. Records are
     * appended (and acknowledged) only after the state covers the segment. */
    if (rc == EFS_OK) rc = state_write(rj.epoch, rj.oldest, seq + 1);
    if (rc != EFS_OK) { close(fd); unlink(path); return rc; }
    if (rj.seg_fd >= 0) close(rj.seg_fd);
    rj.seg_fd = fd;
    rj.seg_seq = seq;
    rj.seg_count = 0;
    rj.segs[rj.nseg++] = (struct rj_seg){.seq = seq, .first = rj.tail, .count = 0};
    return EFS_OK;
}

static struct rj_rec *rec_at(uint64_t i) { return &rj.recs[i % RJ_CAP]; }

/* Append and fsync; records enter the ring only once durable. rj.mu held. */
static int journal_append_locked(const struct efs_reclaim_item *items, const uint32_t *pick,
                                 uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        if (rj.seg_fd < 0 || rj.seg_count >= RJ_SEG_RECORDS) {
            int rc = seg_open_locked();
            if (rc != EFS_OK) return rc;
        }
        uint32_t room = (uint32_t)(RJ_SEG_RECORDS - rj.seg_count), take = n - done;
        if (take > room) take = room;
        uint8_t *buf = malloc((size_t)take * RJ_REC_BYTES);
        if (!buf) return EFS_ERR_NOMEM;
        for (uint32_t k = 0; k < take; k++) {
            const struct efs_reclaim_item *it = &items[pick[done + k]];
            struct rj_rec r = {.kind = it->kind, .fi = it->fragment, .ci = it->chunk,
                               .eid = it->export_id, .ino = it->ino,
                               .gen = it->generation, .seg = rj.seg_seq};
            memcpy(r.sum, it->sum, EFS_HASH_SIZE);
            rec_encode(buf + (size_t)k * RJ_REC_BYTES, &r);
        }
        int rc = write_all(rj.seg_fd, buf, (size_t)take * RJ_REC_BYTES);
        free(buf);
        if (rc == EFS_OK) rc = fsync_fd(rj.seg_fd);
        if (rc != EFS_OK) {
            /* Nothing of this write was acknowledged. Cut the segment back to
             * its acknowledged records so it still verifies once sealed, and
             * never append to it again. If even that fails, refuse every
             * acceptance until a restart verifies (or rebuilds) the journal. */
            off_t good = (off_t)(RJ_HDR_BYTES + rj.seg_count * RJ_REC_BYTES);
            if (ftruncate(rj.seg_fd, good) || fsync_fd(rj.seg_fd) != EFS_OK) {
                rj.wedged = 1;
                fprintf(stderr, "reclaim: journal segment %llu tail unrepairable; acceptance stopped\n",
                        (unsigned long long)rj.seg_seq);
            }
            close(rj.seg_fd);
            rj.seg_fd = -1;
            return rc;
        }
        for (uint32_t k = 0; k < take; k++) {
            const struct efs_reclaim_item *it = &items[pick[done + k]];
            struct rj_rec *r = rec_at(rj.tail++);
            memset(r, 0, sizeof(*r));
            r->kind = it->kind;
            r->fi = it->fragment;
            r->ci = it->chunk;
            memcpy(r->sum, it->sum, EFS_HASH_SIZE);
            r->eid = it->export_id;
            r->ino = it->ino;
            r->gen = it->generation;
            r->seg = rj.seg_seq;
            r->claimed = 1; /* deleters wait until its credit is applied */
            /* A segment has no death-bitmap entry: its object is gone or
             * no longer ACTIVE, which refuses every later PUT. */
            r->checkpointed = it->kind != EFS_RECLAIM_INODE;
        }
        rj.seg_count += take;
        rj.segs[rj.nseg - 1].count += take;
        done += take;
    }
    rj.last_append_us = now_us();
    return EFS_OK;
}

static void credit_record(struct rj_rec *r)
{
    uint64_t payload = 0;
    if (r->kind == EFS_RECLAIM_SUPERSEDED)
        payload = 0; /* its deletion debits `used` itself */
    else if (r->kind == EFS_RECLAIM_SEGMENT)
        payload = server_segment_payload(rj.s, r->eid & 0xffu, r->ino, r->gen);
    else if (server_gc_inode_payload(rj.s, r->eid, r->ino, &payload) != EFS_OK)
        payload = 0; /* uncredited: deletion then debits `used` directly */
    pthread_mutex_lock(&rj.s->lock);
    struct efs_node *ln = server_local_node(rj.s);
    if (ln && payload) {
        if (payload > ln->used) payload = ln->used;
        ln->used -= payload;
        rj.s->reclaim_pending_bytes += payload;
    }
    pthread_mutex_unlock(&rj.s->lock);
    r->credited = payload ? payload : 0;
    if (payload) server_usage_mark_dirty(rj.s);
    __atomic_fetch_add(&rj.credited, payload, __ATOMIC_RELAXED);
}

/* A segment may be accepted only once this node can prove, by a
 * linearizable read of its group, that the object is retired or no longer
 * ACTIVE: states never return to ACTIVE, so after the fence retires no PUT
 * can pass its authority check again. */
static int segment_acceptable(const struct efs_reclaim_item *it)
{
    struct efs_pack_object *o = malloc(sizeof(*o));
    if (!o) return EFS_ERR_NOMEM;
    int rc = server_raft_host_pack_object(it->export_id & 0xffu, it->ino, it->generation, o);
    if (rc == EFS_OK && o->ledger.phase == EFS_PACK_ACTIVE) rc = EFS_ERR_BUSY;
    if (rc == EFS_ERR_NOT_FOUND) rc = EFS_OK;
    free(o);
    return rc;
}

static int accept_internal(const struct efs_reclaim_item *items, uint32_t n, int32_t *rc_out,
                           int bypass)
{
    for (uint32_t i = 0; i < n; i++) rc_out[i] = EFS_ERR_BUSY;
    if (!rj.ready) return EFS_ERR_BUSY;
    /* Segment authority first, outside the journal lock (ReadIndex). */
    uint8_t *refused = calloc(n ? n : 1, 1);
    if (!refused) return EFS_ERR_NOMEM;
    for (uint32_t i = 0; i < n; i++)
        if (items[i].kind == EFS_RECLAIM_SEGMENT && segment_acceptable(&items[i]) != EFS_OK)
            refused[i] = 1;
    if (!bypass && (rj.recovering ||
                    __atomic_load_n(&rj.s->reclaim_fail_closed, __ATOMIC_ACQUIRE))) {
        __atomic_fetch_add(&rj.busy, n, __ATOMIC_RELAXED);
        free(refused);
        return EFS_ERR_BUSY;
    }
    uint32_t *pick = malloc((size_t)(n ? n : 1) * sizeof(*pick));
    if (!pick) { free(refused); return EFS_ERR_NOMEM; }
    uint32_t np = 0;
    pthread_mutex_lock(&rj.mu);
    if (rj.wedged || rj.tail - rj.head + n > RJ_CAP) {
        pthread_mutex_unlock(&rj.mu);
        free(pick); free(refused);
        __atomic_fetch_add(&rj.busy, n, __ATOMIC_RELAXED);
        return EFS_ERR_BUSY;
    }
    for (uint32_t i = 0; i < n; i++) {
        struct rj_rec probe = {.kind = items[i].kind, .eid = items[i].export_id,
                               .ino = items[i].ino, .gen = items[i].generation};
        int dup = !rec_valid(&probe) ||
                  (items[i].kind == EFS_RECLAIM_INODE && efs_ino_is_meta_table(items[i].ino)) ||
                  (items[i].kind == EFS_RECLAIM_SEGMENT && !efs_pack_group_anchor(items[i].export_id & 0xffu));
        if (dup) { rc_out[i] = EFS_ERR_INVAL; continue; }
        if (refused[i]) continue; /* stays BUSY */
        /* A fenced inode or segment is already owned; for a superseded
         * fragment that means its whole inode is being reclaimed. */
        if (server_reclaim_fenced(items[i].export_id, items[i].ino)) { rc_out[i] = EFS_OK; continue; }
        for (uint32_t j = 0; j < np && !dup; j++) {
            const struct efs_reclaim_item *a = &items[pick[j]], *b = &items[i];
            dup = a->kind == b->kind && a->ino == b->ino && a->export_id == b->export_id &&
                  (b->kind != EFS_RECLAIM_SUPERSEDED ||
                   (a->generation == b->generation && a->chunk == b->chunk && a->fragment == b->fragment));
        }
        if (dup) { rc_out[i] = EFS_OK; continue; }
        pick[np++] = i;
    }
    uint64_t first = rj.tail;
    int rc = journal_append_locked(items, pick, np);
    if (rc != EFS_OK) {
        pthread_mutex_unlock(&rj.mu);
        free(pick); free(refused);
        __atomic_fetch_add(&rj.busy, n, __ATOMIC_RELAXED);
        for (uint32_t i = 0; i < n; i++) if (rc_out[i] == EFS_OK) rc_out[i] = EFS_ERR_BUSY;
        return rc;
    }
    /* Fences: under each inode's write guard, after the journal is durable.
     * A superseded fragment needs none: its generation is never written
     * again, and its deletion is checksum-guarded. */
    for (uint32_t k = 0; k < np; k++) {
        const struct efs_reclaim_item *it = &items[pick[k]];
        if (it->kind == EFS_RECLAIM_SUPERSEDED) continue;
        pthread_rwlock_t *g = it->kind == EFS_RECLAIM_SEGMENT
            ? server_segment_guard(it->export_id & 0xffu, it->ino)
            : server_inode_guard(it->export_id, it->ino);
        pthread_rwlock_wrlock(g);
        pthread_rwlock_wrlock(&fence_mu);
        fence_insert_locked(it->export_id, it->ino);
        pthread_rwlock_unlock(&fence_mu);
        server_inode_gc_epoch_bump();
        pthread_rwlock_unlock(g);
    }
    pthread_mutex_unlock(&rj.mu);
    /* Credit after the fence: no PUT can change the inode's bytes now. */
    for (uint64_t i = first; i < first + np; i++) {
        pthread_mutex_lock(&rj.mu);
        struct rj_rec *r = rec_at(i);
        pthread_mutex_unlock(&rj.mu);
        credit_record(r);
    }
    pthread_mutex_lock(&rj.mu);
    for (uint64_t i = first; i < first + np; i++) rec_at(i)->claimed = 0;
    pthread_cond_broadcast(&rj.work);
    pthread_mutex_unlock(&rj.mu);
    for (uint32_t k = 0; k < np; k++) rc_out[pick[k]] = EFS_OK;
    __atomic_fetch_add(&rj.accepted, np, __ATOMIC_RELAXED);
    free(pick); free(refused);
    return EFS_OK;
}

int server_reclaim_accept(struct efsd_server *s, const struct efs_reclaim_item *items,
                          uint32_t n, int32_t *rc)
{
    if (!s || (n && (!items || !rc))) return EFS_ERR_INVAL;
    int r = accept_internal(items, n, rc, 0);
    for (uint32_t i = 0; i < n; i++)
        if (rc[i] == EFS_OK) {
            server_fault_crash_point("reclaim-accepted"); /* journaled, not answered */
            break;
        }
    return r;
}

/* ---- background work ---- */
/* Quota settles once per inode: acceptance moved `credited` from used to
 * pending; unlinks by this deleter were only counted. Bytes the credit
 * counted but someone else removed meanwhile (the chunk GC pass deleting an
 * old generation) were already debited from used, so give them back. */
static void settle_record(const struct rj_rec *r)
{
    if (!r->credited) return;
    pthread_mutex_lock(&rj.s->lock);
    struct efs_node *ln = server_local_node(rj.s);
    rj.s->reclaim_pending_bytes = rj.s->reclaim_pending_bytes >= r->credited
                                      ? rj.s->reclaim_pending_bytes - r->credited : 0;
    if (ln) {
        if (r->removed > r->credited) {
            uint64_t extra = r->removed - r->credited;
            ln->used = ln->used >= extra ? ln->used - extra : 0;
        } else {
            ln->used += r->credited - r->removed;
        }
    }
    pthread_mutex_unlock(&rj.s->lock);
    server_usage_mark_dirty(rj.s);
}

static void *deleter_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&rj.mu);
    while (!rj.stop) {
        /* Claim up to RJ_DELETE_BATCH unclaimed, undeleted records. The
         * cursor only passes deleted records: claimed ones (credit pending,
         * or held by another deleter that may fail) are rescanned. */
        uint64_t ids[RJ_DELETE_BATCH];
        uint32_t n = 0;
        if (rj.delete_cursor < rj.head) rj.delete_cursor = rj.head;
        while (rj.delete_cursor < rj.tail && rec_at(rj.delete_cursor)->deleted) rj.delete_cursor++;
        for (uint64_t i = rj.delete_cursor; i < rj.tail && n < RJ_DELETE_BATCH; i++) {
            struct rj_rec *r = rec_at(i);
            if (!r->deleted && !r->claimed) { r->claimed = 1; ids[n++] = i; }
        }
        if (!n || rj.hold) {
            for (uint32_t k = 0; k < n; k++) rec_at(ids[k])->claimed = 0;
            pthread_cond_wait(&rj.work, &rj.mu);
            continue;
        }
        pthread_mutex_unlock(&rj.mu);
        for (uint32_t k = 0; k < n; k++) {
            struct rj_rec *r;
            pthread_mutex_lock(&rj.mu);
            r = rec_at(ids[k]);
            struct rj_rec copy = *r;
            pthread_mutex_unlock(&rj.mu);
            uint64_t counted = 0;
            /* Directory syncs are deferred to retirement (retire_some). */
            int rc;
            if (copy.kind == EFS_RECLAIM_SUPERSEDED) {
                rc = server_gc_superseded_delete(rj.s, copy.eid, copy.ino, copy.ci, copy.fi,
                                                 copy.gen, copy.sum, 0);
            } else if (copy.kind == EFS_RECLAIM_SEGMENT) {
                rc = server_segment_delete(rj.s, copy.eid & 0xffu, copy.ino, copy.gen, &counted);
                if (!copy.credited && counted) {
                    /* Uncredited: debit what was unlinked directly. */
                    pthread_mutex_lock(&rj.s->lock);
                    struct efs_node *ln = server_local_node(rj.s);
                    if (ln) ln->used = ln->used >= counted ? ln->used - counted : 0;
                    pthread_mutex_unlock(&rj.s->lock);
                    server_usage_mark_dirty(rj.s);
                    counted = 0;
                }
            } else {
                rc = server_gc_inode_delete(rj.s, copy.eid, copy.ino,
                                            copy.credited ? &counted : NULL, 0);
            }
            pthread_mutex_lock(&rj.mu);
            r = rec_at(ids[k]);
            r->removed += counted;
            if (rc == EFS_OK) {
                copy = *r;
                r->deleted = 1;
                rj.deleted++;
            }
            r->claimed = 0;
            pthread_mutex_unlock(&rj.mu);
            if (rc == EFS_OK) {
                settle_record(&copy);
            } else {
                struct timespec ts = {0, 100 * 1000 * 1000}; /* back off on I/O errors */
                nanosleep(&ts, NULL);
            }
        }
        pthread_mutex_lock(&rj.mu);
        pthread_cond_broadcast(&rj.idle);
    }
    pthread_mutex_unlock(&rj.mu);
    return NULL;
}

/* Each checkpoint costs two syncfs on the shared filesystem: run one when
 * RJ_CHECKPOINT_MIN records wait, the oldest waited 1 s, or the journal is
 * half full (retirement must keep up with the cap). */
#define RJ_CHECKPOINT_MIN 8192u
static uint64_t last_checkpoint_us;
static void checkpoint_some(void)
{
    pthread_mutex_lock(&rj.mu);
    if (rj.checkpoint_cursor < rj.head) rj.checkpoint_cursor = rj.head;
    uint64_t waiting = rj.tail - rj.checkpoint_cursor, live = rj.tail - rj.head;
    pthread_mutex_unlock(&rj.mu);
    uint64_t t = now_us();
    if (!waiting || (waiting < RJ_CHECKPOINT_MIN && t - last_checkpoint_us < 1000000 && live < RJ_CAP / 2))
        return;
    last_checkpoint_us = t;
    efs_ino_t *inos = malloc(RJ_CHECKPOINT_BATCH * sizeof(*inos));
    uint64_t *ids = malloc(RJ_CHECKPOINT_BATCH * sizeof(*ids));
    if (!inos || !ids) { free(inos); free(ids); return; }
    for (;;) {
        uint32_t n = 0, eid = 0;
        pthread_mutex_lock(&rj.mu);
        if (rj.checkpoint_cursor < rj.head) rj.checkpoint_cursor = rj.head;
        for (uint64_t i = rj.checkpoint_cursor; i < rj.tail && n < RJ_CHECKPOINT_BATCH; i++) {
            struct rj_rec *r = rec_at(i);
            if (r->checkpointed) continue;
            if (!eid) eid = r->eid;
            if (r->eid != eid) continue;
            inos[n] = r->ino;
            ids[n++] = i;
        }
        pthread_mutex_unlock(&rj.mu);
        if (!n) break;
        if (server_gc_death_set_many(rj.s, eid, inos, n) != EFS_OK) break;
        pthread_mutex_lock(&rj.mu);
        for (uint32_t k = 0; k < n; k++) {
            if (ids[k] >= rj.head) { rec_at(ids[k])->checkpointed = 1; rj.checkpointed++; }
        }
        while (rj.checkpoint_cursor < rj.tail && rec_at(rj.checkpoint_cursor)->checkpointed)
            rj.checkpoint_cursor++;
        pthread_mutex_unlock(&rj.mu);
    }
    free(inos);
    free(ids);
}

/* Retire sealed segments whose records are all checkpointed and deleted.
 * The deleters skip directory syncs, so one syncfs per filesystem makes all
 * of a segment's unlinks durable before its records may disappear. */
static void retire_some(void)
{
    int synced = 0;
    uint64_t covered = 0;
    pthread_mutex_lock(&rj.mu);
    for (;;) {
        if (!rj.nseg) break;
        struct rj_seg *sg = &rj.segs[0];
        int current = rj.seg_fd >= 0 && sg->seq == rj.seg_seq;
        uint64_t idle = now_us() - rj.last_append_us;
        int all = 1;
        for (uint64_t i = sg->first; i < sg->first + sg->count && all; i++) {
            struct rj_rec *r = rec_at(i);
            all = r->checkpointed && r->deleted;
        }
        if (!all) break;
        if (current && idle < 1000000) break;
        if(synced && sg->first+sg->count>covered)break;
        if (!synced) {
            /* Freeze the completed prefix BEFORE sync. Deleters may finish
             * later records while unlocked; those need the next existing
             * batch barrier, never this earlier one. */
            covered=rj.head;
            while(covered<rj.tail && rec_at(covered)->checkpointed && rec_at(covered)->deleted)covered++;
            /* Outside rj.mu: completed records stay completed. */
            pthread_mutex_unlock(&rj.mu);
            int src = server_gc_sync_roots(rj.s);
            pthread_mutex_lock(&rj.mu);
            if (src != EFS_OK) break;
            synced = 1;
            continue; /* re-evaluate segs[0] under the lock */
        }
        current = rj.seg_fd >= 0 && sg->seq == rj.seg_seq;
        if (current) {
            /* Seal an idle, completed current segment so it can retire. */
            close(rj.seg_fd);
            rj.seg_fd = -1;
        }
        uint64_t seq = sg->seq;
        if (state_write(rj.epoch, seq + 1, rj.next) != EFS_OK) break;
        char path[4200];
        seg_path(path, sizeof(path), seq);
        if (unlink(path) && errno != ENOENT) break;
        (void)fsync_dir(rj.dir);
        pthread_rwlock_wrlock(&fence_mu);
        for (uint64_t i = sg->first; i < sg->first + sg->count; i++)
            if (rec_at(i)->kind != EFS_RECLAIM_SUPERSEDED) /* installed no fence */
                fence_remove_locked(rec_at(i)->eid, rec_at(i)->ino);
        pthread_rwlock_unlock(&fence_mu);
        rj.head = sg->first + sg->count;
        rj.retired += sg->count;
        memmove(&rj.segs[0], &rj.segs[1], (rj.nseg - 1) * sizeof(rj.segs[0]));
        rj.nseg--;
    }
    pthread_mutex_unlock(&rj.mu);
}

static uint64_t last_sample_us;
static void *bg_main(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&rj.stop, __ATOMIC_ACQUIRE)) {
        uint64_t t = now_us();
        if (t - last_sample_us >= 500000) { server_phys_sample(rj.s); last_sample_us = t; }
        checkpoint_some();
        retire_some();
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ---- physical admission guard ---- */
/* Runtime floor per filesystem: never let reclaim lag fill it. Startup quota
 * planning keeps its own 20% reserve (storage_capacity.h); this floor is
 * smaller so a filesystem already past 80% (orcd-mini /data1) can still run,
 * and it keeps RJ_JOURNAL_MIN for journal segments and bitmap checkpoints. */
static uint64_t phys_floor(uint64_t total)
{
    uint64_t floor = total / 20;
    if (floor < (1ull << 30)) floor = 1ull << 30;
    return floor + RJ_JOURNAL_MIN;
}
void server_phys_sample(struct efsd_server *s)
{
    if (!s) return;
    uint32_t nroot = s->storage_path_count ? s->storage_path_count : 1;
    dev_t dev[EFS_MAX_STORAGE_PATHS];
    uint64_t usable[EFS_MAX_STORAGE_PATHS];
    uint8_t slot[EFS_MAX_STORAGE_PATHS];
    uint32_t nfs = 0;
    for (uint32_t r = 0; r < nroot; r++) {
        struct stat st;
        struct statvfs fs;
        slot[r] = 0;
        if (stat(s->storage_paths[r], &st) || statvfs(s->storage_paths[r], &fs)) continue;
        uint32_t j;
        for (j = 0; j < nfs && dev[j] != st.st_dev; j++) {}
        if (j == nfs) {
            uint64_t unit = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
            uint64_t total = (uint64_t)fs.f_blocks * unit, avail = (uint64_t)fs.f_bavail * unit;
            uint64_t floor = phys_floor(total);
            dev[nfs] = st.st_dev;
            usable[nfs++] = avail > floor ? avail - floor : 0;
        }
        slot[r] = (uint8_t)(j + 1);
    }
    pthread_mutex_lock(&s->lock);
    for (uint32_t r = 0; r < nroot; r++) s->phys_fs[r] = slot[r];
    for (uint32_t j = 0; j < nfs; j++) {
        s->phys_avail[j] = usable[j];
        s->phys_charged[j] = 0;
    }
    pthread_mutex_unlock(&s->lock);
}
int server_phys_admit_locked(struct efsd_server *s, const char *path, uint64_t bytes)
{
    uint32_t nroot = s->storage_path_count ? s->storage_path_count : 1;
    for (uint32_t r = 0; r < nroot; r++) {
        size_t l = strlen(s->storage_paths[r]);
        if (strncmp(path, s->storage_paths[r], l) || (path[l] && path[l] != '/')) continue;
        uint8_t f = s->phys_fs[r];
        if (!f) return 0; /* not sampled yet (new root, or stat failed) */
        uint64_t *avail = &s->phys_avail[f - 1], *charged = &s->phys_charged[f - 1];
        if (*charged > *avail || bytes > *avail - *charged) {
            s->phys_refused++;
            return -1;
        }
        *charged += bytes;
        return 0;
    }
    return 0; /* not under a storage root (metadata tables) */
}

/* ---- startup: verify, replay or fail closed ---- */
static int load_segment(uint64_t seq, int last)
{
    char path[4200];
    seg_path(path, sizeof(path), seq);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return -1; }
    uint8_t h[RJ_HDR_BYTES], hh[EFS_HASH_SIZE];
    if (pread(fd, h, sizeof(h), 0) != (ssize_t)sizeof(h)) { close(fd); return -1; }
    efs_hash(h, 32, hh);
    uint32_t version = get32(h + 8);
    if (memcmp(h, RJ_HDR_MAGIC, 8) || (version != 1 && version != RJ_VERSION) ||
        get64(h + 16) != rj.epoch ||
        get64(h + 24) != seq || memcmp(hh, h + 32, EFS_HASH_SIZE)) { close(fd); return -1; }
    /* A version 1 segment is sealed: appends always open a new segment. */
    uint32_t rsz = version == 1 ? RJ_REC_V1_BYTES : RJ_REC_BYTES;
    uint64_t bytes = (uint64_t)st.st_size - RJ_HDR_BYTES, nrec = bytes / rsz;
    if (!last && bytes % rsz) { close(fd); return -1; }
    rj.segs[rj.nseg++] = (struct rj_seg){.seq = seq, .first = rj.tail, .count = 0};
    uint8_t b[RJ_REC_BYTES];
    int bad = 0;
    uint64_t valid = 0;
    for (uint64_t k = 0; k < nrec; k++) {
        if (pread(fd, b, rsz, (off_t)(RJ_HDR_BYTES + k * rsz)) != (ssize_t)rsz) {
            close(fd); return -1;
        }
        struct rj_rec r;
        if (rec_decode(b, version, seq, &r)) {
            if (!last) { close(fd); return -1; }
            bad = 1; /* possible torn tail: every later record must be invalid too */
            continue;
        }
        if (bad) { close(fd); return -1; }
        if (rj.tail - rj.head >= RJ_CAP) { close(fd); return -1; }
        r.checkpointed = r.kind != EFS_RECLAIM_INODE;
        *rec_at(rj.tail++) = r;
        rj.segs[rj.nseg - 1].count++;
        valid = k + 1;
    }
    close(fd);
    if (last && (uint64_t)st.st_size != RJ_HDR_BYTES + valid * rsz) {
        /* A torn tail was never acknowledged (the fsync did not return).
         * Cut it so this segment verifies cleanly once it is no longer last. */
        int wfd = open(path, O_WRONLY | O_CLOEXEC);
        if (wfd < 0) return -1;
        int rc = ftruncate(wfd, (off_t)(RJ_HDR_BYTES + valid * rsz)) ? -1 : 0;
        if (!rc && fsync_fd(wfd)) rc = -1;
        close(wfd);
        if (rc) return -1;
        fprintf(stderr, "reclaim: segment %llu torn tail cut at %llu records\n",
                (unsigned long long)seq, (unsigned long long)valid);
    }
    return 0;
}

static int verify_and_replay(void)
{
    int st = state_read();
    DIR *d = opendir(rj.dir);
    if (st < 0) { if (d) closedir(d); return -1; }
    int any_seg = 0;
    uint64_t max_seen = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t l = strlen(de->d_name);
            if (l != 20 || strcmp(de->d_name + 16, ".seg")) continue;
            any_seg = 1;
            uint64_t seq = strtoull(de->d_name, NULL, 16);
            if (seq + 1 > max_seen) max_seen = seq + 1;
            if (st == 1 && seq < rj.oldest) {
                char path[4200];
                seg_path(path, sizeof(path), seq);
                unlink(path); /* retired; state advanced before the crash */
            }
        }
        closedir(d);
    }
    if (st == 0) {
        if (any_seg) return -1; /* segments without a state record */
        if (mkdir(rj.dir, 0755) && errno != EEXIST) return -1;
        uint64_t epoch = ((uint64_t)time(NULL) << 20) ^ (uint64_t)getpid() ^ now_us();
        return state_write(epoch, 0, 0) == EFS_OK ? 0 : -1;
    }
    if (max_seen == rj.next + 1) {
        /* Crash between creating segment `next` and advancing the state. No
         * record was appended to it (appends follow the state update). */
        char path[4200];
        struct stat sst;
        seg_path(path, sizeof(path), rj.next);
        if (!stat(path, &sst) && sst.st_size <= (off_t)RJ_HDR_BYTES) {
            unlink(path);
            (void)fsync_dir(rj.dir);
            max_seen = rj.next;
        }
    }
    if (max_seen > rj.next) return -1; /* state older than the journal */
    for (uint64_t seq = rj.oldest; seq < rj.next; seq++)
        if (load_segment(seq, seq + 1 == rj.next)) return -1;
    return 0;
}

/* Recovery: new epoch, scan local inode directories, fence proven-dead ones. */
static int decode_seg4(const char *n, uint64_t *v)
{
    if (strlen(n) != 4) return -1;
    uint64_t x = 0;
    for (int i = 0; i < 4; i++) { if (n[i] < '0' || n[i] > '9') return -1; x = x * 10 + (uint64_t)(n[i] - '0'); }
    *v = x;
    return 0;
}
/* Accept a recovery batch, waiting out the journal cap (deleters drain it). */
static void recover_flush(struct efs_reclaim_item *batch, uint32_t *nb)
{
    int32_t rcs[1024];
    uint32_t n = *nb;
    while (n && !__atomic_load_n(&rj.stop, __ATOMIC_ACQUIRE)) {
        accept_internal(batch, n, rcs, 1);
        uint32_t keep = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (rcs[i] == EFS_OK || rcs[i] == EFS_ERR_INVAL) rj.recovered++;
            else batch[keep++] = batch[i];
        }
        n = keep;
        if (n) {
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
    *nb = 0;
}
static void recover_walk(const char *path, int level, uint64_t prefix, uint32_t eid,
                         struct efs_reclaim_item *batch, uint32_t *nb)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && !__atomic_load_n(&rj.stop, __ATOMIC_ACQUIRE)) {
        uint64_t v;
        if (decode_seg4(de->d_name, &v)) continue;
        uint64_t p = prefix * 10000ull + v;
        char child[8192];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (level < 4) { recover_walk(child, level + 1, p, eid, batch, nb); continue; }
        efs_ino_t ino = (efs_ino_t)p;
        if (ino <= EFS_ROOT_INO || efs_ino_is_meta_table(ino)) continue;
        int dead = 0;
        /* Transient doubt (no quorum yet) is retried; an inode this node
         * cannot judge (its group is not hosted here) stays unfenced, which
         * risks only a leaked fragment, never a lost live one. */
        for (int tries = 0; tries < 600 && !__atomic_load_n(&rj.stop, __ATOMIC_ACQUIRE); tries++) {
            int rc = server_raft_host_inode_reaped(ino, &dead);
            if (rc == EFS_OK || rc == EFS_ERR_INVAL) break;
            dead = 0;
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
        if (!dead) continue;
        batch[*nb] = (struct efs_reclaim_item){.export_id = eid, .ino = ino, .generation = 0};
        if (++*nb == 1024) recover_flush(batch, nb);
    }
    closedir(d);
}
/* Segment files whose object is absent or no longer ACTIVE (proven by a
 * linearizable read) lost their journal record with the journal: fence and
 * delete them again. Doubt keeps the file. */
struct recover_seg { struct efs_reclaim_item *batch; uint32_t *nb; };
static void recover_segment(void *arg, uint32_t group, uint64_t id, uint64_t gen)
{
    struct recover_seg *rs = arg;
    struct efs_reclaim_item it = {.export_id = EFS_RECLAIM_SEGMENT_EID(group), .ino = id,
                                  .generation = gen, .kind = EFS_RECLAIM_SEGMENT};
    for (uint32_t i = 0; i < *rs->nb; i++)
        if (rs->batch[i].kind == it.kind && rs->batch[i].ino == id && rs->batch[i].export_id == it.export_id)
            return; /* another fragment of the same segment */
    int rc = EFS_ERR_BUSY;
    for (int tries = 0; tries < 600 && !__atomic_load_n(&rj.stop, __ATOMIC_ACQUIRE); tries++) {
        rc = segment_acceptable(&it);
        if (rc == EFS_OK || rc == EFS_ERR_BUSY || rc == EFS_ERR_INVAL) break;
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (rc != EFS_OK) return;
    rs->batch[*rs->nb] = it;
    if (++*rs->nb == 1024) recover_flush(rs->batch, rs->nb);
}
static void *recover_main(void *arg)
{
    (void)arg;
    while (!server_raft_host_active() && !__atomic_load_n(&rj.stop, __ATOMIC_ACQUIRE)) {
        struct timespec ts = {0, 200 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    struct efs_reclaim_item *batch = malloc(1024 * sizeof(*batch));
    uint32_t nb = 0;
    if (!batch) return NULL;
    for (uint32_t r = 0; r < (rj.s->storage_path_count ? rj.s->storage_path_count : 1); r++) {
        char base[4200];
        snprintf(base, sizeof(base), "%s/data/exports", rj.s->storage_paths[r]);
        DIR *d = opendir(base);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            char *end;
            unsigned long eid = strtoul(de->d_name, &end, 10);
            if (!eid || *end) continue;
            char path[8192];
            snprintf(path, sizeof(path), "%s/%s", base, de->d_name);
            recover_walk(path, 0, 0, (uint32_t)eid, batch, &nb);
        }
        closedir(d);
    }
    struct recover_seg rs = {batch, &nb};
    server_segment_scan(rj.s, recover_segment, &rs);
    recover_flush(batch, &nb);
    free(batch);
    if (__atomic_load_n(&rj.stop, __ATOMIC_ACQUIRE)) return NULL; /* still fail closed */
    fprintf(stderr, "reclaim: recovery complete, %llu dead inodes fenced; writes re-admitted\n",
            (unsigned long long)rj.recovered);
    pthread_mutex_lock(&rj.mu);
    rj.recovering = 0;
    pthread_mutex_unlock(&rj.mu);
    __atomic_store_n(&rj.s->reclaim_fail_closed, 0, __ATOMIC_RELEASE);
    server_inode_gc_epoch_bump();
    return NULL;
}

int server_reclaim_init(struct efsd_server *s)
{
    if (!s) return EFS_ERR_INVAL;
    rj.s = s;
    snprintf(rj.dir, sizeof(rj.dir), "%s/meta/gc-journal", s->storage_paths[0]);
    snprintf(rj.root, sizeof(rj.root), "%s", s->storage_paths[0]);
    rj.recs = calloc(RJ_CAP, sizeof(*rj.recs));
    fence_cap = 1;
    while (fence_cap < 2ull * RJ_CAP) fence_cap <<= 1;
    fence_keys = calloc(fence_cap, sizeof(*fence_keys));
    fence_eids = calloc(fence_cap, sizeof(*fence_eids));
    if (!rj.recs || !fence_keys || !fence_eids) return EFS_ERR_NOMEM;
    char meta[4200];
    snprintf(meta, sizeof(meta), "%s/meta", s->storage_paths[0]);
    mkdir(meta, 0755);
    if (verify_and_replay() == 0) {
        for (uint64_t i = rj.head; i < rj.tail; i++) {
            struct rj_rec *r = rec_at(i);
            if (r->kind != EFS_RECLAIM_SUPERSEDED) fence_insert_locked(r->eid, r->ino);
        }
        server_inode_gc_epoch_bump();
        rj.ready = 1;
        for (uint64_t i = rj.head; i < rj.tail; i++) credit_record(rec_at(i));
        fprintf(stderr, "reclaim: journal epoch=%llu segments=%u records=%llu replayed\n",
                (unsigned long long)rj.epoch, rj.nseg,
                (unsigned long long)(rj.tail - rj.head));
    } else {
        /* Fail closed, then rebuild from a scan under a fresh epoch. */
        __atomic_store_n(&s->reclaim_fail_closed, 1, __ATOMIC_RELEASE);
        fprintf(stderr, "reclaim: journal verification FAILED in %s; data PUTs refused until recovery\n",
                rj.dir);
        /* K30: a second failure within the same second must not collide
         * with the earlier aside (rename onto a non-empty directory). */
        char aside[4300];
        long long when = (long long)time(NULL);
        int moved = 0;
        for (unsigned n = 0; n < 1000 && !moved; n++) {
            if (n) snprintf(aside, sizeof(aside), "%s.broken.%lld.%u", rj.dir, when, n);
            else snprintf(aside, sizeof(aside), "%s.broken.%lld", rj.dir, when);
            if (!rename(rj.dir, aside) || errno == ENOENT) moved = 1;
            else if (errno != EEXIST && errno != ENOTEMPTY) return EFS_ERR_IO;
        }
        if (!moved) return EFS_ERR_IO;
        rj.head = rj.tail = 0;
        rj.nseg = 0;
        if (mkdir(rj.dir, 0755) && errno != EEXIST) return EFS_ERR_IO;
        uint64_t epoch = ((uint64_t)time(NULL) << 20) ^ (uint64_t)getpid() ^ now_us();
        if (state_write(epoch, 0, 0) != EFS_OK) return EFS_ERR_IO;
        rj.recovering = 1;
        rj.ready = 1;
    }
    if (pthread_create(&rj.bg, NULL, bg_main, NULL)) return EFS_ERR_IO;
    for (int i = 0; i < RJ_DELETERS; i++)
        if (pthread_create(&rj.del[i], NULL, deleter_main, NULL)) return EFS_ERR_IO;
    rj.threads = 1;
    if (rj.recovering) {
        if (pthread_create(&rj.rec_tid, NULL, recover_main, NULL)) return EFS_ERR_IO;
        rj.rec_started = 1;
    }
    pthread_mutex_lock(&rj.mu);
    pthread_cond_broadcast(&rj.work);
    pthread_mutex_unlock(&rj.mu);
    return EFS_OK;
}

void server_reclaim_shutdown(struct efsd_server *s)
{
    (void)s;
    if (!rj.threads) return;
    pthread_mutex_lock(&rj.mu);
    rj.stop = 1;
    pthread_cond_broadcast(&rj.work);
    pthread_mutex_unlock(&rj.mu);
    pthread_join(rj.bg, NULL);
    for (int i = 0; i < RJ_DELETERS; i++) pthread_join(rj.del[i], NULL);
    if (rj.rec_started) pthread_join(rj.rec_tid, NULL);
    rj.rec_started = 0;
    rj.threads = 0;
    pthread_mutex_lock(&rj.mu);
    if (rj.seg_fd >= 0) { close(rj.seg_fd); rj.seg_fd = -1; }
    pthread_mutex_unlock(&rj.mu);
}

void server_reclaim_status(struct efsd_server *s, struct efs_reclaim_status *out)
{
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&rj.mu);
    out->accepted = rj.accepted;
    out->deleted = rj.deleted;
    out->checkpointed = rj.checkpointed;
    out->retired = rj.retired;
    out->journal_records = rj.tail - rj.head;
    out->journal_segments = rj.nseg;
    out->journal_epoch = rj.epoch;
    out->recovering = (uint32_t)rj.recovering;
    out->recovered = rj.recovered;
    pthread_mutex_unlock(&rj.mu);
    out->busy = __atomic_load_n(&rj.busy, __ATOMIC_RELAXED);
    out->credited_bytes = __atomic_load_n(&rj.credited, __ATOMIC_RELAXED);
    if (s) {
        pthread_mutex_lock(&s->lock);
        out->pending_bytes = s->reclaim_pending_bytes;
        for (uint32_t j = 0; j < EFS_MAX_STORAGE_PATHS; j++)
            if (s->phys_avail[j] > s->phys_charged[j])
                out->phys_avail_bytes += s->phys_avail[j] - s->phys_charged[j];
        out->phys_refused = s->phys_refused;
        pthread_mutex_unlock(&s->lock);
        out->fail_closed = (uint32_t)__atomic_load_n(&s->reclaim_fail_closed, __ATOMIC_ACQUIRE);
    }
}
