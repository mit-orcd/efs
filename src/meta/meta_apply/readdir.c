/* meta_apply/readdir.c: READDIR scans and RMDIR. */

#include "apply_internal.h"
#include "rows.h"
#include "lookup.h"
#include "init.h"
#include "xattr.h"
#include "namespace.h"

/* --- readdir ---------------------------------------------------------
 *
 * Ordered keys make a directory listing a range scan: dentries for one
 * parent are a contiguous key range, so a page costs the page. A spread
 * directory is up to 64 such ranges on 64 shards, and because a name hashes
 * to exactly one dir lane, visiting the lanes in order cannot produce a
 * duplicate — no cross-lane dedup set is needed.
 *
 * SPLITTING is the one case where a name can exist twice: on its hashed lane
 * and still on the pre-split local shard. The hashed side always wins (a
 * mutation during the split writes the hashed side, using a tombstone to
 * record a delete), so the local pass consults the hashed side per name and
 * skips anything found there — live (already returned) or tombstoned
 * (deleted). That is what stops the migration from resurrecting a name.
 *
 * Dir lane 0 is the directory's own inode shard (directory.md), and a dentry
 * key carries the shard but not the layout — so for the 1-in-64 names that
 * hash to lane 0, the hashed key and the pre-split local key are the same
 * key. Such a name is born in its final location and never migrates. Two
 * consequences, both easy to get wrong in opposite directions: the local
 * pass must not treat those names as claimed by the hashed side (they would
 * vanish), and during SPLITTING lane 0 must not be scanned as a lane on top
 * of the local pass (every unmigrated name would come back twice).
 *
 * So source 0 is always the inode shard's range: the whole directory when
 * LOCAL, the local pass when SPLITTING, lane 0 when HASHED. It must come
 * first. A listing is many pages and the migrator runs between them, moving
 * names local -> hashed only. Local first means a name the local pass has
 * not reached yet can only move to a lane that is still ahead of the cursor.
 * Scanning the lanes first lost every name that moved into an already
 * scanned lane before the local pass got to it: a run of names at the
 * migration frontier per listing (W125 C7, 1M-file directory). The cost is
 * the reverse window: a name listed by the local pass and then migrated is
 * listed again by its lane. Readdir is a weak listing; callers drop repeated
 * names. The same numbering keeps a cookie valid across LOCAL -> SPLITTING
 * -> HASHED, which can happen mid-listing. */

#define DENTRY_KEY_PREFIX 11 /* shard + kind + parent; name follows */

struct dir_scan {
    struct efs_kv *kv;
    struct efs_meta_dir_ent *out;
    uint32_t max;
    uint32_t n;
    uint32_t *remaining;
    char last[EFS_MAX_NAME];
    int have_last;
    int full; /* stopped because the page filled, not because the range ended */
    int rc;
};

static int dir_scan_cb(void *user, const uint8_t *key, uint32_t klen,
                       const uint8_t *val, uint32_t vlen)
{
    struct dir_scan *s = user;
    uint32_t nl;

    if (s->remaining) {
        if (!*s->remaining) {
            s->full = 1;
            return 1;
        }
        --*s->remaining;
    }
    if (klen <= DENTRY_KEY_PREFIX)
        return 0; /* the prefix itself is not an entry */
    nl = klen - DENTRY_KEY_PREFIX;
    if (nl >= EFS_MAX_NAME) {
        s->rc = efs_kv_decode_result(s->kv, key, klen, EFS_ERR_PROTO);
        return s->rc;
    }
    if (s->n >= s->max) {
        s->full = 1; /* stop; the cursor resumes in this same source */
        return 1;
    }
    memcpy(s->out[s->n].name, key + DENTRY_KEY_PREFIX, nl);
    s->out[s->n].name[nl] = 0;
    int decoded = unpack_dentry(val, vlen, &s->out[s->n].d);
    if (decoded != EFS_OK) {
        s->rc = efs_kv_decode_result(s->kv, key, klen, decoded);
        return s->rc;
    }
    memcpy(s->last, s->out[s->n].name, nl + 1);
    s->have_last = 1;
    if (s->out[s->n].d.type == EFS_META_DENT_TOMBSTONE)
        return 0; /* recorded as progress, never returned to the caller */
    s->n++;
    return 0;
}

/* Scans one source's range, resuming strictly after cur->name when set. */
static int dir_scan_one(struct efs_kv *kv, uint32_t shard, efs_ino_t dir,
                        const char *after, struct dir_scan *s)
{
    uint8_t pre[EFS_KV_KEY_MAX], start[EFS_KV_KEY_MAX];
    uint32_t pl = 0, sl = 0;
    int rc;

    s->kv = kv;

    rc = efs_kv_key_dentry_prefix(shard, dir, pre, &pl);
    if (rc != EFS_OK)
        return rc;
    if (after && after[0]) {
        rc = efs_kv_key_dentry(shard, dir, after, start, &sl);
        if (rc != EFS_OK)
            return rc;
        /* One 0 byte past the last name is its immediate successor and still
         * precedes any longer name beginning with it, so nothing is skipped
         * or repeated. */
        if (sl + 1 > EFS_KV_KEY_MAX)
            return EFS_ERR_INVAL;
        start[sl++] = 0;
    }
    rc = efs_kv_scan_from(kv, pre, pl, sl ? start : NULL, sl, dir_scan_cb, s);
    if (rc != EFS_OK && rc != 1)
        return rc;
    return s->rc;
}

/* A committed destination may still be an EXCL intent after the source's
 * deletion has materialized. Scan intents BEFORE base keys: resolution moves
 * intent -> base atomically, so that order cannot lose a stable name. Merge
 * only through the smaller full-stream frontier; an invisible intent must
 * never move the cookie past an unseen name in the other stream. */
#define DIR_TX_SCAN_MAX 256
#define DIR_TX_SCAN_MIN 6
struct dir_names {
    struct efs_kv *kv;
    char names[DIR_TX_SCAN_MAX][EFS_MAX_NAME];
    uint32_t prefix, count, limit;
    uint32_t *remaining;
    int full, rc, intent;
};

static int dir_names_cb(void *user, const uint8_t *key, uint32_t kl,
                        const uint8_t *val, uint32_t vl)
{
    struct dir_names *a = user;
    if (a->remaining) {
        if (!*a->remaining) { a->full = 1; return 1; }
        --*a->remaining;
    }
    if (kl <= a->prefix)
        return 0;
    uint32_t nl = kl - a->prefix;
    if (nl >= EFS_MAX_NAME || memchr(key + a->prefix, 0, nl)) {
        a->rc = efs_kv_decode_result(a->kv, key, kl, EFS_ERR_PROTO);
        return a->rc;
    }
    if (a->count == a->limit) { a->full = 1; return 1; }
    if (!a->intent) {
        struct efs_meta_dentry d;
        int rc = unpack_dentry(val, vl, &d);
        if (rc != EFS_OK) {
            a->rc = efs_kv_decode_result(a->kv, key, kl, rc);
            return a->rc;
        }
    }
    memcpy(a->names[a->count], key + a->prefix, nl);
    a->names[a->count++][nl] = 0;
    return 0;
}

static int dir_names_scan(struct efs_kv *kv, uint32_t shard, efs_ino_t dir,
                           const char *after, struct dir_names *a)
{
    uint8_t pre[EFS_KV_KEY_MAX], start[EFS_KV_KEY_MAX], tmp[EFS_KV_KEY_MAX];
    uint32_t pl = 0, sl = 0;
    int rc = efs_kv_key_dentry_prefix(shard, dir, pre, &pl);
    if (rc != EFS_OK) return rc;
    if (after && after[0]) {
        rc = efs_kv_key_dentry(shard, dir, after, start, &sl);
        if (rc != EFS_OK) return rc;
    }
    if (a->intent) {
        rc = efs_kv_key_intent(pre, pl, tmp, &pl);
        if (rc != EFS_OK) return rc;
        memcpy(pre, tmp, pl);
        if (sl) {
            rc = efs_kv_key_intent(start, sl, tmp, &sl);
            if (rc != EFS_OK) return rc;
            memcpy(start, tmp, sl);
        }
    }
    if (sl) {
        if (sl == EFS_KV_KEY_MAX) return EFS_ERR_INVAL;
        start[sl++] = 0;
    }
    a->kv = kv;
    a->prefix = pl;
    rc = efs_kv_scan_from(kv, pre, pl, sl ? start : NULL, sl, dir_names_cb, a);
    return rc == EFS_OK || rc == 1 ? a->rc : rc;
}

static int dir_scan_one_tx(struct efs_kv *kv, uint32_t shard, efs_ino_t dir,
                            const char *after, struct dir_scan *s,
                            efs_txn_coord_fn coord, void *ctx)
{
    struct dir_names *a = calloc(2, sizeof(*a));
    if (!a) return EFS_ERR_NOMEM;
    uint32_t limit = s->max - s->n;
    if (limit > DIR_TX_SCAN_MAX) limit = DIR_TX_SCAN_MAX;
    /* Each stream reserves its candidate limit plus a lookahead and the
     * possible prefix key. Neither stream can spend the other's budget. */
    if (s->remaining && limit > (*s->remaining / 2 - 2))
        limit = *s->remaining / 2 - 2;
    a[0].intent = 1;
    for (unsigned i = 0; i < 2; ++i) {
        a[i].limit = limit;
        a[i].remaining = s->remaining;
    }
    int rc = dir_names_scan(kv, shard, dir, after, &a[0]);
    if (rc == EFS_OK) rc = dir_names_scan(kv, shard, dir, after, &a[1]);
    if (rc != EFS_OK) { free(a); return rc; }
    const char *frontier = NULL;
    for (unsigned k = 0; k < 2; ++k)
        if (a[k].full && a[k].count &&
            (!frontier || strcmp(a[k].names[a[k].count - 1], frontier) < 0))
            frontier = a[k].names[a[k].count - 1];
    uint32_t i = 0, j = 0;
    while (i < a[0].count || j < a[1].count) {
        const char *name;
        int cmp = i == a[0].count ? 1 : j == a[1].count ? -1 :
                  strcmp(a[0].names[i], a[1].names[j]);
        name = cmp <= 0 ? a[0].names[i] : a[1].names[j];
        if ((frontier && strcmp(name, frontier) > 0) || s->n == s->max)
            break;
        struct efs_meta_dentry d;
        rc = dent_get_tx(kv, shard, dir, name, coord, ctx, &d);
        if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND) break;
        strcpy(s->last, name);
        s->have_last = 1;
        if (rc == EFS_OK && d.type != EFS_META_DENT_TOMBSTONE) {
            strcpy(s->out[s->n].name, name);
            s->out[s->n++].d = d;
        }
        rc = EFS_OK;
        if (cmp <= 0) ++i;
        if (cmp >= 0) ++j;
    }
    s->full = a[0].full || a[1].full || i < a[0].count || j < a[1].count;
    free(a);
    return rc;
}

/* During SPLITTING the hashed side is authoritative for a name. */
static int hashed_side_claims(struct efs_kv *kv, efs_ino_t dir, const char *name,
                              uint64_t used_shards, int *claimed)
{
    struct efs_meta_dentry d;
    int rc;

    *claimed = 0;
    /* A lane-0 name has no separate hashed copy to defer to — the record in
     * hand IS its hashed record. Asking the KV would find that same record
     * and drop the name from the listing. */
    uint8_t lane = efs_kv_dir_lane(name);
    if (lane == 0)
        return EFS_OK;
    rc = dent_get(kv, efs_kv_dentry_shard(dir, name, EFS_META_LAYOUT_HASHED),
                  dir, name, &d);
    if (rc == EFS_OK) {
        /* A cross-group copy may materialize before its parent bitmap OR.
         * Defer a live local name only to a source this scan will visit.
         * A newly added bit triggers the final row recheck/page restart.
         * Tombstones always dominate, even before the bit materializes. */
        *claimed = d.type == EFS_META_DENT_TOMBSTONE ||
                   (used_shards & (1ULL << lane)) != 0;
        return EFS_OK;
    }
    if (rc == EFS_ERR_NOT_FOUND) {
        *claimed = 0;
        return EFS_OK;
    }
    return rc;
}

#if EFS_FAULTS
#include <stdio.h>
#include <unistd.h>
/* Pin a real migration between READDIR's inode cut and its source scan.
 * No metadata is changed by this hook; the test issues a normal raft-dir
 * migration on another worker, then releases this bounded pause. */
static void k48_readdir_layout_pause(efs_ino_t dir)
{
    const char *path = getenv("EFS_FAULT_READDIR_LAYOUT_FILE");
    char ready[EFS_MAX_PATH], release[EFS_MAX_PATH];
    unsigned long long target = 0;
    FILE *f;
    if (!path || !(f = fopen(path, "r")))
        return;
    int parsed = fscanf(f, "%llu", &target);
    fclose(f);
    if (parsed != 1 || target != dir)
        return;
    if (snprintf(ready, sizeof(ready), "%s.ready", path) >= (int)sizeof(ready) ||
        snprintf(release, sizeof(release), "%s.release", path) >= (int)sizeof(release))
        return;
    if (rename(path, ready))
        return;
    /* sync: test-only fault or debug hook */
    fprintf(stderr, "fault k48-readdir: paused inode=%llu\n", target);
    for (unsigned i = 0; i < 1000 && access(release, F_OK); i++)
        usleep(10000);
    /* sync: test-only fault or debug hook */
    fprintf(stderr, "fault k48-readdir: released inode=%llu ready=%d\n",
            target, access(release, F_OK) == 0);
}
#endif

static int readdir_impl(struct efs_kv *kv, efs_ino_t dir,
                           struct efs_meta_dir_cursor *cur,
                           struct efs_meta_dir_ent *out, uint32_t max,
                           uint32_t *n, uint32_t *remaining,
                           efs_txn_coord_fn coord, void *ctx)
{
    struct efs_meta_row row, after;
    struct efs_meta_dir_cursor begin;
    unsigned retries = 0;
    struct dir_scan s;
    uint32_t last_src;
    int rc;

    if (!kv || !cur || !out || !n || max == 0 || dir == 0)
        return EFS_ERR_INVAL;
    *n = 0;
    if (cur->done)
        return EFS_OK;
    begin = *cur;
retry:
    *cur = begin;
    rc = efs_meta_apply_get_inode(kv, dir, &row);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(row.mode))
        return EFS_ERR_INVAL; /* as create_file does for a non-dir parent */

#if EFS_FAULTS
    k48_readdir_layout_pause(dir);
#endif

    last_src = row.layout == EFS_META_LAYOUT_LOCAL ? 0 : EFS_META_LANES - 1;

    memset(&s, 0, sizeof(s));
    s.out = out;
    s.remaining = remaining;
    s.max = max;
    while (cur->src <= last_src && s.n < max &&
           (!remaining || *remaining >= (coord ? DIR_TX_SCAN_MIN : 1))) {
        uint32_t shard, base = s.n;
        int local_pass = 0;

        if (cur->src == 0) {
            /* LOCAL, the SPLITTING local pass and HASHED lane 0 are all
             * this one range. The local pass runs whatever the bitmap says. */
            shard = efs_kv_inode_shard(dir);
            local_pass = row.layout == EFS_META_LAYOUT_SPLITTING;
            if (row.layout == EFS_META_LAYOUT_HASHED &&
                (row.used_shards & 1ULL) == 0) {
                cur->src++;
                cur->name[0] = 0;
                continue;
            }
        } else {
            /* A lane this directory has never used holds no keys, and the
             * bitmap records every first use — including the migrator's. */
            if ((row.used_shards & (1ULL << cur->src)) == 0) {
                cur->src++;
                cur->name[0] = 0;
                continue;
            }
            shard = efs_kv_lane_shard(dir, (uint8_t)cur->src);
        }

        s.have_last = 0;
        s.last[0] = 0;
        s.full = 0;
        rc = coord ? dir_scan_one_tx(kv, shard, dir, cur->name, &s, coord, ctx) :
                     dir_scan_one(kv, shard, dir, cur->name, &s);
        if (rc != EFS_OK)
            return rc;
        if (local_pass) {
            uint32_t i, keep = base;

            /* Only this source's entries; earlier sources are already final. */
            for (i = base; i < s.n; i++) {
                int claimed = 0;

                rc = hashed_side_claims(kv, dir, out[i].name,
                                        row.used_shards, &claimed);
                if (rc != EFS_OK)
                    return rc;
                if (claimed)
                    continue;
                if (keep != i)
                    out[keep] = out[i];
                keep++;
            }
            s.n = keep;
        }
        if (s.have_last)
            memcpy(cur->name, s.last, strlen(s.last) + 1);
        if (s.full) {
            /* More remains in this source. Continue through shadowed-only
             * chunks while budget remains. Budget exhaustion may return no
             * names with an advancing cursor: callers must use done for EOF. */
            if (s.n > 0 || (remaining && *remaining < (coord ? DIR_TX_SCAN_MIN : 1)))
                break;
            continue;
        }
        /* This source is exhausted: advance with a fresh name position. */
        cur->src++;
        cur->name[0] = 0;
    }
    if (cur->src > last_src)
        cur->done = 1;
    /* Migration may run between the inode read and a source scan. A new
     * lane absent from row.used_shards must not be skipped after its LOCAL
     * name has moved (or was suppressed by hashed_side_claims). Likewise a
     * LOCAL -> SPLITTING transition must not turn an old source-0-only cut
     * into EOF. Publish this page/cookie only under the same layout cut.
     * The bitmap gains at most 64 bits and layout transitions twice, so
     * valid monotonic migration cannot exhaust this bounded retry count. */
    rc = efs_meta_apply_get_inode(kv, dir, &after);
    if (rc != EFS_OK)
        return rc;
    if (after.layout != row.layout || after.used_shards != row.used_shards ||
        after.layout_epoch != row.layout_epoch) {
        if (++retries > EFS_META_LANES + 2 ||
            (remaining && *remaining < (coord ? DIR_TX_SCAN_MIN : 1))) {
            *cur = begin;
            return EFS_ERR_BUSY;
        }
        goto retry;
    }
    *n = s.n;
    return EFS_OK;
}

int efs_meta_apply_readdir_bounded(struct efs_kv *kv, efs_ino_t dir,
                           struct efs_meta_dir_cursor *cur,
                           struct efs_meta_dir_ent *out, uint32_t max,
                           uint32_t *n, uint32_t *remaining)
{
    return readdir_impl(kv, dir, cur, out, max, n, remaining, NULL, NULL);
}

int efs_meta_apply_readdir_tx_bounded(struct efs_kv *kv, efs_ino_t dir,
                           struct efs_meta_dir_cursor *cur,
                           struct efs_meta_dir_ent *out, uint32_t max,
                           uint32_t *n, uint32_t *remaining,
                           efs_txn_coord_fn coord, void *ctx)
{
    if (!coord || (remaining && *remaining < DIR_TX_SCAN_MIN))
        return EFS_ERR_BUSY;
    return readdir_impl(kv, dir, cur, out, max, n, remaining, coord, ctx);
}

int efs_meta_apply_readdir(struct efs_kv *kv, efs_ino_t dir,
                           struct efs_meta_dir_cursor *cur,
                           struct efs_meta_dir_ent *out, uint32_t max,
                           uint32_t *n)
{
    return readdir_impl(kv, dir, cur, out, max, n, NULL, NULL, NULL);
}

static int dir_has_live(struct efs_kv *kv, uint32_t shard, efs_ino_t dir)
{
    struct efs_meta_dir_ent one;
    struct dir_scan s;
    int rc;

    memset(&s, 0, sizeof(s));
    memset(&one, 0, sizeof(one));
    s.out = &one;
    s.max = 1;
    rc = dir_scan_one(kv, shard, dir, NULL, &s);
    if (rc != EFS_OK)
        return rc;
    if (s.rc != EFS_OK)
        return s.rc;
    return s.n > 0 ? EFS_ERR_NOT_EMPTY : EFS_OK;
}

static int dir_empty_now(struct efs_kv *kv, const struct efs_meta_row *row)
{
    uint8_t lane;
    int rc;

    if (row->nlink > 2)
        return EFS_ERR_NOT_EMPTY;
    if (row->layout == EFS_META_LAYOUT_SPLITTING)
        return EFS_ERR_BUSY;
    if (row->layout == EFS_META_LAYOUT_LOCAL)
        return dir_has_live(kv, efs_kv_inode_shard(row->ino), row->ino);
    if (row->used_shards == 0)
        return EFS_OK;
    for (lane = 0; lane < EFS_META_LANES; lane++) {
        if ((row->used_shards & (1ull << lane)) == 0)
            continue;
        rc = dir_has_live(kv, efs_kv_lane_shard(row->ino, lane), row->ino);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

/* "Empty now" is not "nobody is adding a child": a cross-group CREATE /
 * MKDIR / LINK into `row` holds only intents until it resolves — a dentry
 * EXCL (invisible to dir_has_live), a REDUCE_ADD on the dseq witness and
 * a REDUCE_INO on the row itself. The log path deletes rows unversioned,
 * so it has to honour those intents the way PREPARE does: BUSY. The
 * probed keys are all local by construction — the log path only runs
 * when every participant shard is on this group. */
static int dir_txn_busy(struct efs_kv *kv, const struct efs_meta_row *row)
{
    uint8_t k[EFS_KV_KEY_MAX];
    uint32_t kl = 0;
    uint8_t lane;
    int rc;

    rc = efs_kv_key_inode(efs_kv_inode_shard(row->ino), row->ino, k, &kl);
    if (rc == EFS_OK)
        rc = efs_txn_key_busy(kv, k, kl);
    if (rc != EFS_OK)
        return rc;
    if (row->layout == EFS_META_LAYOUT_LOCAL) {
        rc = efs_kv_key_dseq(efs_kv_inode_shard(row->ino), row->ino, 0, k, &kl);
        if (rc == EFS_OK)
            rc = efs_txn_key_busy(kv, k, kl);
        return rc;
    }
    for (lane = 0; lane < EFS_META_LANES; lane++) {
        if ((row->used_shards & (1ull << lane)) == 0)
            continue;
        rc = efs_kv_key_dseq(efs_kv_lane_shard(row->ino, lane), row->ino, lane,
                             k, &kl);
        if (rc == EFS_OK)
            rc = efs_txn_key_busy(kv, k, kl);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

int efs_meta_apply_rmdir(struct efs_kv *kv, efs_ino_t parent, const char *name,
                         uint64_t now)
{
    return efs_meta_apply_rmdir_op(kv, parent, name, now, NULL);
}

int efs_meta_apply_rmdir_op(struct efs_kv *kv, efs_ino_t parent, const char *name,
                            uint64_t now, const struct efs_opid_req *q)
{
    struct efs_meta_dentry dent;
    struct efs_meta_row row, prow;
    struct efs_opid_reply rep;
    uint8_t k_loc[EFS_KV_KEY_MAX], k_hash[EFS_KV_KEY_MAX], v_tomb[DENT_VAL];
    uint8_t k_ino[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX], v_par[INO_VAL];
    uint8_t k_ln[EFS_KV_KEY_MAX], v_ln[LANE_VAL];
    uint8_t k_dseq[EFS_KV_KEY_MAX], v_dseq[8];
    uint8_t k_opid[EFS_KV_KEY_MAX], v_opid[EFS_OPID_VAL_MAX];
    uint8_t k_xa[EFS_KV_KEY_MAX];
    uint32_t kl = 0, kh = 0, ki = 0, kp = 0, kln = 0, ks = 0, kxa = 0, dsh;
    struct efs_kv_item it[12];
    uint32_t n = 0;
    int rc, held, xa = 0, stamp_lane = 0;
    uint8_t dseq_lane;

    if (!kv || !name || parent == 0)
        return EFS_ERR_INVAL;
    if (q && opid_hosted(&q->id)) {
        rc = efs_meta_apply_get_inode(kv, parent, &prow);
        if (rc != EFS_OK)
            return rc;
        dsh = efs_kv_dentry_shard(parent, name, prow.layout);
        rc = efs_meta_apply_opid_probe(kv, dsh, &q->id, &rep);
        if (rc < 0)
            return rc;
        if (rc)
            return rep.rc;
    }
    rc = efs_meta_apply_resolve(kv, parent, name, &dent, &row);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(row.mode) || row.ino == EFS_ROOT_INO)
        return EFS_ERR_INVAL;
    rc = dir_empty_now(kv, &row);
    if (rc != EFS_OK)
        return rc;
    rc = dir_txn_busy(kv, &row);
    if (rc != EFS_OK)
        return rc;
    held = efs_lease_any(kv, row.ino, row.generation);
    if (held < 0)
        return held;
    if (held)
        return EFS_ERR_BUSY;
    rc = efs_meta_apply_get_inode(kv, parent, &prow);
    if (rc != EFS_OK)
        return rc;
    if (prow.nlink < 3)
        return EFS_ERR_PROTO;
    prow.nlink--;
    if (prow.layout == EFS_META_LAYOUT_LOCAL) {
        prow.base_mtime = max_u64(prow.base_mtime, now);
        prow.base_ctime = max_u64(prow.base_ctime, now);
        efs_meta_dir_note_entry(&prow, -1);
    } else {
        efs_dir_spread_seen(&prow);
        rc = dir_lane_stamp(kv, &prow, name, now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
    }
    pack_inode(v_par, &prow);
    rc = efs_kv_key_inode(efs_kv_inode_shard(parent), parent, k_par, &kp);
    if (rc != EFS_OK)
        return rc;

    memset(it, 0, sizeof(it));
    rc = dentry_drop_items(kv, &prow, parent, name, k_loc, &kl, k_hash, &kh,
                           v_tomb, it, &n);
    if (rc != EFS_OK)
        return rc;
    /* W36 (same as unlink): a dir RENAME holds an EXCL intent on this
     * dentry until it resolves; dir_txn_busy above probed the child's
     * row and dseq, not the name being dropped. */
    rc = efs_txn_key_busy(kv, k_loc, kl);
    if (rc == EFS_OK && kh && !(kh == kl && memcmp(k_hash, k_loc, kl) == 0))
        rc = efs_txn_key_busy(kv, k_hash, kh);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_DEL;
    it[n].key = k_ino;
    it[n].klen = ki;
    n++;
    rc = xattr_del_item(kv, row.ino, k_xa, &kxa, &it[n], &xa);
    if (rc != EFS_OK)
        return rc;
    n += (uint32_t)xa;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_par;
    it[n].klen = kp;
    it[n].val = v_par;
    it[n].vlen = INO_VAL;
    n++;
    if (stamp_lane) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ln;
        it[n].klen = kln;
        it[n].val = v_ln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    dseq_lane = prow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    dsh = efs_kv_dentry_shard(parent, name, prow.layout);
    rc = dseq_bump(kv, dsh, parent, dseq_lane, k_dseq, &ks, v_dseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
    n++;
    if (q) {
        memset(&rep, 0, sizeof(rep));
        rep.rc = EFS_OK;
        rep.ino = row.ino;
        rc = opid_item(kv, dsh, q, &rep, k_opid, v_opid, &it[n]);
        if (rc < 0)
            return rc;
        n += (uint32_t)rc;
    }
    return meta_write_batch(kv, it, n);
}
