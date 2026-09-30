#include "kv_lsm_internal.h"
#include "efs/kv_key.h"
#include "efs/raft.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define KV_LSM_MAGIC 0x4B564C31u /* KVL1 */

void kv_seg_path(const struct kv_lsm *l, int level, uint64_t seq, char *out,
                 size_t n)
{
    snprintf(out, n, "%s/seg-%d-%010llu.sst", l->dir, level,
             (unsigned long long)seq);
}

/* The caller names a directory, not a tree it has to pre-build; a store
 * per shard under a per-node root is the expected layout. */
static int mkdir_p(const char *path)
{
    char buf[KV_LSM_PATH_MAX];
    size_t i, n = strlen(path);

    if (n == 0 || n >= sizeof(buf))
        return EFS_ERR_INVAL;
    memcpy(buf, path, n + 1);
    for (i = 1; i <= n; i++) {
        char c = buf[i];

        if (c != '/' && c != '\0')
            continue;
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            return EFS_ERR_IO;
        buf[i] = c;
    }
    return EFS_OK;
}

void kv_sync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY);

    if (fd < 0)
        return;
    fsync(fd);
    close(fd);
}

/* --- memtable -------------------------------------------------------- */

/* The key lives in the entry's own allocation: the memtable binary
 * search dereferences e[mid] then its key on every probe, and two
 * dependent cache misses per probe was 13 % of efsd (perf, Sep 26). */
static void ent_free(struct kv_ent *e)
{
    if (!e)
        return;
    free(e->val);
    free(e);
}

void kv_mtab_clear(struct kv_mtab *m)
{
    uint32_t i;

    for (i = 0; i < m->n; i++)
        ent_free(m->e[i]);
    free(m->e);
    free(m->pfx);
    m->e = NULL;
    m->pfx = NULL;
    m->n = 0;
    m->cap = 0;
    m->bytes = 0;
}

/* First 8 key bytes as a big-endian integer, zero-padded. Ordered like
 * kv_key_cmp except that a key shorter than 8 bytes ties with a longer
 * key whose tail is zeros; a tie falls through to the full compare. The
 * memtable keeps these in a contiguous side array so a binary search
 * probes ~200 KiB of L2-resident integers and touches an entry (one
 * cache miss) only on a prefix tie. */
static uint64_t key_pfx(const uint8_t *key, uint32_t klen)
{
    uint64_t v = 0;
    uint32_t i, n = klen < 8 ? klen : 8;

    for (i = 0; i < n; i++)
        v = (v << 8) | key[i];
    return v << (8 * (8 - n));
}

/* Index of key, or the insertion point with *found = 0. */
uint32_t kv_mtab_pos(const struct kv_mtab *m, const uint8_t *key,
                         uint32_t klen, int *found)
{
    uint32_t lo = 0, hi = m->n;
    uint64_t kp = key_pfx(key, klen);

    *found = 0;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint64_t mp = m->pfx[mid];
        int c = mp < kp ? -1 : mp > kp ? 1 :
                kv_key_cmp(m->e[mid]->key, m->e[mid]->klen, key, klen);
        if (c == 0) {
            *found = 1;
            return mid;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int kv_mtab_set(struct kv_mtab *m, uint8_t op, const uint8_t *key,
                    uint32_t klen, const uint8_t *val, uint32_t vlen)
{
    struct kv_ent *e;
    uint32_t at;
    int found;

    if (op == KV_OP_DEL)
        vlen = 0;
    at = kv_mtab_pos(m, key, klen, &found);
    if (found) {
        uint8_t *nv = NULL;

        if (vlen) {
            nv = malloc(vlen);
            if (!nv)
                return EFS_ERR_NOMEM;
            memcpy(nv, val, vlen);
        }
        e = m->e[at];
        m->bytes -= e->vlen;
        free(e->val);
        e->val = nv;
        e->vlen = vlen;
        e->op = op;
        m->bytes += vlen;
        return EFS_OK;
    }
    if (m->n == m->cap) {
        uint32_t cap = m->cap ? m->cap * 2 : 64;
        struct kv_ent **p = realloc(m->e, cap * sizeof(*p));
        uint64_t *q;
        if (!p)
            return EFS_ERR_NOMEM;
        m->e = p;
        q = realloc(m->pfx, cap * sizeof(*q));
        if (!q)
            return EFS_ERR_NOMEM;
        m->pfx = q;
        m->cap = cap;
    }
    e = calloc(1, sizeof(*e) + klen);
    if (!e)
        return EFS_ERR_NOMEM;
    e->key = (uint8_t *)(e + 1);
    memcpy(e->key, key, klen);
    e->klen = klen;
    if (vlen) {
        e->val = malloc(vlen);
        if (!e->val) {
            ent_free(e);
            return EFS_ERR_NOMEM;
        }
        memcpy(e->val, val, vlen);
    }
    e->vlen = vlen;
    e->op = op;
    if (at < m->n) {
        memmove(&m->e[at + 1], &m->e[at], (m->n - at) * sizeof(*m->e));
        memmove(&m->pfx[at + 1], &m->pfx[at], (m->n - at) * sizeof(*m->pfx));
    }
    m->e[at] = e;
    m->pfx[at] = key_pfx(key, klen);
    m->n++;
    m->bytes += klen + vlen + sizeof(*e) + sizeof(e) + sizeof(*m->pfx);
    return EFS_OK;
}

/* --- live set -------------------------------------------------------- */

static void drop_segs(struct kv_lsm *l)
{
    uint32_t i;

    for (i = 0; i < l->n_l0; i++)
        kv_seg_close(l->l0[i].seg);
    for (i = 0; i < l->n_l1; i++)
        kv_seg_close(l->l1[i].seg);
    l->n_l0 = 0;
    l->n_l1 = 0;
}

int kv_manifest_write(struct kv_lsm *l)
{
    char path[KV_LSM_PATH_MAX + 32];
    char tmp[KV_LSM_PATH_MAX + 32];
    FILE *f;
    uint32_t i;

    snprintf(path, sizeof(path), "%s/MANIFEST", l->dir);
    snprintf(tmp, sizeof(tmp), "%s/MANIFEST.tmp", l->dir);
    f = fopen(tmp, "wb");
    if (!f)
        return EFS_ERR_IO;
    if (fprintf(f, "KVLSM1 %llu\n", (unsigned long long)l->next_seq) < 0)
        goto fail;
    for (i = 0; i < l->n_l0; i++)
        if (fprintf(f, "0 %llu\n", (unsigned long long)l->l0[i].seq) < 0)
            goto fail;
    for (i = 0; i < l->n_l1; i++)
        if (fprintf(f, "1 %llu\n", (unsigned long long)l->l1[i].seq) < 0)
            goto fail;
    if (fflush(f) != 0 || fsync(fileno(f)) != 0)
        goto fail;
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return EFS_ERR_IO;
    }
    kv_sync_dir(l->dir);
    return EFS_OK;

fail:
    fclose(f);
    unlink(tmp);
    return EFS_ERR_IO;
}

/* Any .sst the MANIFEST does not list is a crashed flush or compaction
 * output; the MANIFEST rename is the only commit point. */
static void gc_orphans(struct kv_lsm *l)
{
    DIR *d = opendir(l->dir);
    struct dirent *de;

    if (!d)
        return;
    while ((de = readdir(d))) {
        int level = -1;
        unsigned long long seq = 0;
        uint32_t i;
        int live = 0;
        char path[KV_LSM_PATH_MAX + 64];

        if (sscanf(de->d_name, "seg-%d-%llu.sst", &level, &seq) != 2) {
            size_t n = strlen(de->d_name);
            if (n > 4 && strcmp(de->d_name + n - 4, ".tmp") == 0) {
                snprintf(path, sizeof(path), "%s/%s", l->dir, de->d_name);
                unlink(path);
            }
            continue;
        }
        for (i = 0; i < l->n_l0 && !live; i++)
            live = l->l0[i].seq == seq && level == 0;
        for (i = 0; i < l->n_l1 && !live; i++)
            live = l->l1[i].seq == seq && level == 1;
        if (!live) {
            snprintf(path, sizeof(path), "%s/%s", l->dir, de->d_name);
            unlink(path);
        }
    }
    closedir(d);
}

int kv_l0_reserve(struct kv_lsm *l, uint32_t need)
{
    struct seg_slot *p;
    uint32_t cap;

    if (need <= l->l0_cap)
        return EFS_OK;
    cap = l->l0_cap ? l->l0_cap : 16u;
    while (cap < need) {
        if (cap > (1u << 20))
            return EFS_ERR_NOMEM;
        cap *= 2u;
    }
    p = realloc(l->l0, (size_t)cap * sizeof(*p));
    if (!p)
        return EFS_ERR_NOMEM;
    l->l0 = p;
    l->l0_cap = cap;
    return EFS_OK;
}

uint64_t kv_l0_bytes(const struct kv_lsm *l)
{
    uint64_t n = 0;
    uint32_t i;

    for (i = 0; i < l->n_l0; i++)
        n += kv_seg_data_bytes(l->l0[i].seg);
    return n;
}

int kv_l1_reserve(struct kv_lsm *l, uint32_t need)
{
    struct seg_slot *p;
    uint32_t cap;

    if (need <= l->l1_cap)
        return EFS_OK;
    cap = l->l1_cap ? l->l1_cap : 16u;
    while (cap < need) {
        if (cap > (1u << 30))
            return EFS_ERR_NOMEM;
        cap *= 2u;
    }
    p = realloc(l->l1, (size_t)cap * sizeof(*p));
    if (!p)
        return EFS_ERR_NOMEM;
    l->l1 = p;
    l->l1_cap = cap;
    return EFS_OK;
}

int kv_l1_cmp(const void *a, const void *b)
{
    const struct seg_slot *x = a, *y = b;
    const uint8_t *xk = NULL, *yk = NULL;
    uint32_t xl = 0, yl = 0;

    if (kv_seg_first_key(x->seg, &xk, &xl) != EFS_OK)
        return 1;
    if (kv_seg_first_key(y->seg, &yk, &yl) != EFS_OK)
        return -1;
    return kv_key_cmp(xk, xl, yk, yl);
}

static int manifest_load(struct kv_lsm *l)
{
    char path[KV_LSM_PATH_MAX + 32];
    char spath[KV_LSM_PATH_MAX + 64];
    FILE *f;
    char line[128];
    unsigned long long ns = 0;
    int rc = EFS_OK;

    snprintf(path, sizeof(path), "%s/MANIFEST", l->dir);
    f = fopen(path, "rb");
    if (!f)
        return EFS_OK;
    if (!fgets(line, sizeof(line), f) || sscanf(line, "KVLSM1 %llu", &ns) != 1) {
        fclose(f);
        return EFS_ERR_PROTO;
    }
    l->next_seq = ns;
    while (fgets(line, sizeof(line), f)) {
        int level;
        unsigned long long seq;
        struct kv_seg *s = NULL;

        if (sscanf(line, "%d %llu", &level, &seq) != 2) {
            rc = EFS_ERR_PROTO;
            break;
        }
        if (level != 0 && level != 1) {
            rc = EFS_ERR_PROTO;
            break;
        }
        /* L0 and L1 are both growable lists. Admission is L0 bytes. */
        if (level == 0) {
            rc = kv_l0_reserve(l, l->n_l0 + 1);
            if (rc != EFS_OK)
                break;
        }
        if (level == 1) {
            rc = kv_l1_reserve(l, l->n_l1 + 1);
            if (rc != EFS_OK)
                break;
        }
        kv_seg_path(l, level, seq, spath, sizeof(spath));
        rc = kv_seg_open(spath, &s);
        if (rc != EFS_OK)
            break;
        if (level == 0) {
            l->l0[l->n_l0].seg = s;
            l->l0[l->n_l0].seq = seq;
            l->l0[l->n_l0].level = 0;
            l->n_l0++;
        } else {
            l->l1[l->n_l1].seg = s;
            l->l1[l->n_l1].seq = seq;
            l->l1[l->n_l1].level = 1;
            l->n_l1++;
        }
    }
    fclose(f);
    if (rc != EFS_OK)
        return rc;
    /* L0 newest first: the MANIFEST writes them in that order already, but
     * sort so a hand-edited or older manifest cannot invert precedence. */
    if (l->n_l0 > 1) {
        uint32_t i, j;
        for (i = 0; i < l->n_l0; i++)
            for (j = i + 1; j < l->n_l0; j++)
                if (l->l0[j].seq > l->l0[i].seq) {
                    struct seg_slot t = l->l0[i];
                    l->l0[i] = l->l0[j];
                    l->l0[j] = t;
                }
    }
    if (l->n_l1 > 1)
        qsort(l->l1, l->n_l1, sizeof(l->l1[0]), kv_l1_cmp);
    return EFS_OK;
}

/* --- reads ----------------------------------------------------------- */

/* EFS_OK with *op, or NOT_FOUND when no level speaks about the key.
 * Caller holds l->mu. A segment miss drops the lock around the pread
 * so concurrent gets overlap; the segment is pinned so compaction can
 * unlink it without closing the fd. The memtable is newer than every
 * segment and is rechecked after each pread. */
static int lookup_mt(struct kv_lsm *l, const uint8_t *key, uint32_t klen,
                     struct kv_buf *out, uint8_t *op)
{
    int found;
    uint32_t at = kv_mtab_pos(&l->mt, key, klen, &found);

    if (!found)
        return EFS_ERR_NOT_FOUND;
    *op = l->mt.e[at]->op;
    return kv_buf_set(out, l->mt.e[at]->val, l->mt.e[at]->vlen);
}

struct lsm_view {
    uint32_t n0, n1;
    struct kv_seg *a, *b;
};

static void lsm_view_get(struct kv_lsm *l, struct lsm_view *v)
{
    v->n0 = l->n_l0;
    v->n1 = l->n_l1;
    v->a = v->n0 ? l->l0[0].seg : NULL;
    v->b = v->n1 ? l->l1[0].seg : NULL;
}

static int lsm_view_same(const struct lsm_view *x, const struct lsm_view *y)
{
    return x->n0 == y->n0 && x->n1 == y->n1 && x->a == y->a && x->b == y->b;
}

static int lookup_locked(struct kv_lsm *l, const uint8_t *key, uint32_t klen,
                         struct kv_buf *out, uint8_t *op)
{
    uint32_t i;
    int rc = lookup_mt(l, key, klen, out, op);

    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    for (i = 0; i < l->n_l0; i++) {
        rc = kv_seg_get(l->l0[i].seg, key, klen, out, op);
        if (rc != EFS_ERR_NOT_FOUND)
            return rc;
    }
    for (i = 0; i < l->n_l1; i++) {
        rc = kv_seg_get(l->l1[i].seg, key, klen, out, op);
        if (rc != EFS_ERR_NOT_FOUND)
            return rc;
    }
    return EFS_ERR_NOT_FOUND;
}

static int lookup(struct kv_lsm *l, const uint8_t *key, uint32_t klen,
                  struct kv_buf *out, uint8_t *op)
{
    int guard = 0;

    for (;;) {
        struct lsm_view snap;
        uint32_t i, n;
        int rc;

        if (guard++ > 16)
            return lookup_locked(l, key, klen, out, op);
        rc = lookup_mt(l, key, klen, out, op);
        if (rc != EFS_ERR_NOT_FOUND)
            return rc;
        lsm_view_get(l, &snap);
        n = l->n_l0 + l->n_l1;
        for (i = 0; i < n; i++) {
            struct kv_seg *s = i < l->n_l0 ? l->l0[i].seg
                                           : l->l1[i - l->n_l0].seg;
            struct kv_seg_io io;
            uint8_t *blk = NULL;
            int need_io = 0;

            rc = kv_seg_probe(s, key, klen, out, op, &need_io, &io);
            if (rc != EFS_OK || !need_io) {
                if (rc != EFS_ERR_NOT_FOUND)
                    return rc;
                continue;
            }
            kv_seg_pin(s);
            pthread_mutex_unlock(&l->mu);
            rc = kv_seg_read(&io, &blk);
            pthread_mutex_lock(&l->mu);
            if (rc != EFS_OK) {
                kv_seg_unpin(s);
                return rc;
            }
            {
                struct lsm_view now;

                lsm_view_get(l, &now);
                rc = kv_seg_install(s, &io, blk, key, klen, out, op);
                kv_seg_unpin(s);
                if (!lsm_view_same(&snap, &now) || rc == EFS_ERR_AGAIN)
                    goto restart;
            }
            /* A put that landed during the pread is in the memtable and
             * is newer than this segment. A miss here still has to walk
             * older segments: the name is absent only if every level
             * says so. */
            {
                int mrc = lookup_mt(l, key, klen, out, op);

                if (mrc != EFS_ERR_NOT_FOUND)
                    return mrc;
            }
            if (rc != EFS_ERR_NOT_FOUND)
                return rc;
        }
        rc = lookup_mt(l, key, klen, out, op);
        if (rc != EFS_ERR_NOT_FOUND)
            return rc;
        {
            struct lsm_view now;

            lsm_view_get(l, &now);
            if (!lsm_view_same(&snap, &now))
                goto restart;
        }
        return EFS_ERR_NOT_FOUND;
    restart:
        ;
    }
}

int kv_msrc_advance(struct kv_lsm *l, struct msrc *s, const uint8_t *lower,
                    uint32_t lower_len)
{
    for (;;) {
        if (!s->it) {
            if (s->mi >= l->mt.n) {
                s->done = 1;
                return EFS_OK;
            }
            s->key = l->mt.e[s->mi]->key;
            s->klen = l->mt.e[s->mi]->klen;
            s->val = l->mt.e[s->mi]->val;
            s->vlen = l->mt.e[s->mi]->vlen;
            s->op = l->mt.e[s->mi]->op;
            s->mi++;
        } else {
            int rc = kv_seg_iter_next(s->it, &s->key, &s->klen, &s->val,
                                      &s->vlen, &s->op);
            if (rc == EFS_ERR_NOT_FOUND) {
                s->done = 1;
                return EFS_OK;
            }
            if (rc != EFS_OK)
                return rc;
        }
        if (lower_len == 0 ||
            kv_key_cmp(s->key, s->klen, lower, lower_len) >= 0)
            return EFS_OK;
    }
}

/* Merges memtable and every segment in precedence order. Newest wins per
 * key; tombstones are consumed, never emitted.
 *
 * `prefix` bounds the range and `start` (optional) is where to begin inside
 * it, so resuming a paged scan seeks every source instead of replaying the
 * range. The termination test stays on `prefix`: `start` moves the cursor,
 * it never widens or narrows what qualifies. */
static int merge_scan(struct kv_lsm *l, const uint8_t *prefix, uint32_t plen,
                      const uint8_t *start, uint32_t slen,
                      int (*cb)(void *user, const uint8_t *key, uint32_t klen,
                                const uint8_t *val, uint32_t vlen),
                      void *user)
{
    struct msrc stack_src[KV_LSM_MAX_SEGS * 2 + 1];
    struct msrc *src = stack_src;
    const uint8_t *seek = slen ? start : prefix;
    uint32_t seek_len = slen ? slen : plen;
    uint32_t nsrc = 0, i, cap = KV_LSM_MAX_SEGS * 2 + 1;
    uint32_t need = l->n_l0 + l->n_l1 + 1;
    int src_heap = 0;
    int rc = EFS_OK;

    if (need > cap) {
        src = calloc(need, sizeof(*src));
        if (!src)
            return EFS_ERR_NOMEM;
        src_heap = 1;
        cap = need;
    } else {
        memset(src, 0, sizeof(stack_src));
    }
    src[nsrc].mi = 0;
    if (l->mt.n && seek_len) {
        int found;
        src[nsrc].mi = kv_mtab_pos(&l->mt, seek, seek_len, &found);
    }
    nsrc++;
    for (i = 0; i < l->n_l0 + l->n_l1; i++) {
        struct seg_slot *sl = i < l->n_l0 ? &l->l0[i] : &l->l1[i - l->n_l0];

        /* L1 is range-partitioned: a 3-byte txn-record prefix lives in
         * one or two of its segments. Opening the rest cost one pread
         * each under l->mu on every PREPARE/RESOLVE apply. */
        if (seek_len && kv_seg_excludes(sl->seg, seek, seek_len, prefix, plen))
            continue;
        if (nsrc >= cap) {
            rc = EFS_ERR_NOMEM;
            goto out;
        }
        rc = kv_seg_iter_open(sl->seg, &src[nsrc].it);
        if (rc != EFS_OK)
            goto out;
        rc = kv_seg_iter_seek(src[nsrc].it, seek, seek_len);
        if (rc != EFS_OK)
            goto out;
        nsrc++;
    }
    for (i = 0; i < nsrc; i++) {
        rc = kv_msrc_advance(l, &src[i], seek, seek_len);
        if (rc != EFS_OK)
            goto out;
    }
    for (;;) {
        uint32_t win = nsrc;

        for (i = 0; i < nsrc; i++) {
            if (src[i].done)
                continue;
            if (win == nsrc ||
                kv_key_cmp(src[i].key, src[i].klen, src[win].key,
                           src[win].klen) < 0)
                win = i;
        }
        if (win == nsrc)
            break;
        if (!kv_has_prefix(src[win].key, src[win].klen, prefix, plen))
            break; /* ordered, so nothing further can match */
        if (src[win].op == KV_OP_PUT) {
            rc = cb(user, src[win].key, src[win].klen, src[win].val,
                    src[win].vlen);
            if (rc != 0)
                goto out;
        }
        for (i = 0; i < nsrc; i++) {
            if (src[i].done || i == win)
                continue;
            if (kv_key_cmp(src[i].key, src[i].klen, src[win].key,
                           src[win].klen) != 0)
                continue;
            rc = kv_msrc_advance(l, &src[i], seek, seek_len);
            if (rc != EFS_OK)
                goto out;
        }
        rc = kv_msrc_advance(l, &src[win], seek, seek_len);
        if (rc != EFS_OK)
            goto out;
    }

out:
    for (i = 0; i < nsrc; i++)
        kv_seg_iter_close(src[i].it);
    if (src_heap)
        free(src);
    return rc;
}

/* --- efs_kv_ops ------------------------------------------------------ */

static int lsm_batch(void *ctx, const struct efs_kv_item *items, uint32_t n)
{
    struct kv_lsm *l = ctx;
    uint64_t seq = 0;
    uint32_t i;
    int rc, arc;

    if (!l || (n > 0 && !items))
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&l->mu);
    if (l->fail_next_batch) {
        l->fail_next_batch = 0;
        pthread_mutex_unlock(&l->mu);
        return EFS_ERR_IO;
    }
    if (l->io_failed) {
        pthread_mutex_unlock(&l->mu);
        return EFS_ERR_IO;
    }
    pthread_mutex_unlock(&l->mu);
    if (n == 0)
        return EFS_OK;
    /* Durable first unless a sync-hold is open: then the record is written
     * and applied, and kv_wal_hold(0) fsyncs the group. Concurrent batches
     * still share one fsync when hold is off. */
    arc = kv_wal_append(l->wal, items, n, &seq);
    if (arc != EFS_OK && seq == 0)
        return arc;
    pthread_mutex_lock(&l->mu);
    /* Apply in log order: a crash replays the WAL, so the visible order must
     * be the logged order even when several batches shared one fsync. */
    while (l->apply_next != seq && !l->io_failed)
        pthread_cond_wait(&l->cv, &l->mu);
    if (l->apply_next != seq) {
        pthread_mutex_unlock(&l->mu);
        return EFS_ERR_IO;
    }
    if (arc != EFS_OK)
        l->io_failed = 1;
    rc = arc;
    for (i = 0; i < n && rc == EFS_OK; i++) {
        uint8_t op = items[i].op == EFS_KV_PUT ? KV_OP_PUT : KV_OP_DEL;
        rc = kv_mtab_set(&l->mt, op, items[i].key, items[i].klen, items[i].val,
                      items[i].vlen);
        /* The record is already durable, so a half-applied batch means RAM
         * now disagrees with what a restart would rebuild. */
        if (rc != EFS_OK)
            l->io_failed = 1;
    }
    if (rc == EFS_OK)
        rc = kv_maybe_flush_locked(l);
    l->apply_next = seq + 1;
    pthread_cond_broadcast(&l->cv);
    pthread_mutex_unlock(&l->mu);
    return rc;
}

static int lsm_put(void *ctx, const uint8_t *key, uint32_t klen,
                   const uint8_t *val, uint32_t vlen)
{
    struct efs_kv_item it;

    if (!key || klen == 0 || (vlen > 0 && !val))
        return EFS_ERR_INVAL;
    it.op = EFS_KV_PUT;
    it.key = key;
    it.klen = klen;
    it.val = val;
    it.vlen = vlen;
    return lsm_batch(ctx, &it, 1);
}

static int lsm_get(void *ctx, const uint8_t *key, uint32_t klen, uint8_t *val,
                   uint32_t *vlen)
{
    struct kv_lsm *l = ctx;
    uint8_t op = 0;
    int rc;

    if (!l || !key || klen == 0 || !vlen)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&l->mu);
    rc = lookup(l, key, klen, &l->scratch, &op);
    if (rc == EFS_OK && op == KV_OP_DEL)
        rc = EFS_ERR_NOT_FOUND;
    if (rc == EFS_OK) {
        uint32_t need = l->scratch.len;

        if (*vlen < need || (need > 0 && !val)) {
            *vlen = need;
            rc = EFS_ERR_INVAL;
        } else {
            if (need)
                memcpy(val, l->scratch.p, need);
            *vlen = need;
        }
    }
    pthread_mutex_unlock(&l->mu);
    return rc;
}

static int lsm_del(void *ctx, const uint8_t *key, uint32_t klen)
{
    struct kv_lsm *l = ctx;
    struct efs_kv_item it;
    uint8_t op = 0;
    int rc;

    if (!l || !key || klen == 0)
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&l->mu);
    rc = lookup(l, key, klen, &l->scratch, &op);
    pthread_mutex_unlock(&l->mu);
    if (rc == EFS_OK && op == KV_OP_DEL)
        rc = EFS_ERR_NOT_FOUND;
    if (rc != EFS_OK)
        return rc;
    it.op = EFS_KV_DEL;
    it.key = key;
    it.klen = klen;
    it.val = NULL;
    it.vlen = 0;
    return lsm_batch(ctx, &it, 1);
}

static int lsm_scan_from(void *ctx, const uint8_t *prefix, uint32_t plen,
                         const uint8_t *start, uint32_t slen,
                         int (*cb)(void *user, const uint8_t *key,
                                   uint32_t klen, const uint8_t *val,
                                   uint32_t vlen),
                         void *user)
{
    struct kv_lsm *l = ctx;
    int rc;

    if (!l || !cb || (plen > 0 && !prefix) || (slen > 0 && !start))
        return EFS_ERR_INVAL;
    pthread_mutex_lock(&l->mu);
    rc = merge_scan(l, prefix, plen, start, slen, cb, user);
    pthread_mutex_unlock(&l->mu);
    return rc;
}

static int lsm_scan_prefix(void *ctx, const uint8_t *prefix, uint32_t plen,
                           int (*cb)(void *user, const uint8_t *key,
                                     uint32_t klen, const uint8_t *val,
                                     uint32_t vlen),
                           void *user)
{
    return lsm_scan_from(ctx, prefix, plen, NULL, 0, cb, user);
}

static int lsm_scan(void *ctx,
                    int (*cb)(void *user, const uint8_t *key, uint32_t klen,
                              const uint8_t *val, uint32_t vlen),
                    void *user)
{
    return lsm_scan_prefix(ctx, NULL, 0, cb, user);
}

static void lsm_destroy(void *ctx)
{
    struct kv_lsm *l = ctx;

    if (!l)
        return;
    kv_compactor_stop(l);
    kv_wal_close(l->wal);
    drop_segs(l);
    free(l->l0);
    l->l0 = NULL;
    l->l0_cap = 0;
    free(l->l1);
    l->l1 = NULL;
    l->l1_cap = 0;
    kv_mtab_clear(&l->mt);
    kv_buf_free(&l->scratch);
    pthread_cond_destroy(&l->compact_cv);
    pthread_cond_destroy(&l->cv);
    pthread_mutex_destroy(&l->mu);
    free(l);
}

static const struct efs_kv_ops lsm_ops = {
    .put = lsm_put,
    .get = lsm_get,
    .del = lsm_del,
    .scan = lsm_scan,
    .scan_prefix = lsm_scan_prefix,
    .scan_from = lsm_scan_from,
    .batch = lsm_batch,
    .destroy = lsm_destroy,
};

static int replay_item(void *user, uint8_t op, const uint8_t *key,
                       uint32_t klen, const uint8_t *val, uint32_t vlen)
{
    struct kv_lsm *l = user;

    if (op != KV_OP_PUT && op != KV_OP_DEL)
        return EFS_ERR_PROTO;
    return kv_mtab_set(&l->mt, op, key, klen, val, vlen);
}

struct efs_kv *efs_kv_lsm_open(const char *dir, const struct efs_kv_lsm_cfg *cfg)
{
    struct efs_kv *kv;
    struct kv_lsm *l;
    pthread_mutexattr_t mattr;
    char wal_path[KV_LSM_PATH_MAX + 32];
    size_t dlen;

    if (!dir)
        return NULL;
    dlen = strlen(dir);
    if (dlen == 0 || dlen >= KV_LSM_PATH_MAX)
        return NULL;
    if (mkdir_p(dir) != EFS_OK)
        return NULL;
    kv = calloc(1, sizeof(*kv));
    l = calloc(1, sizeof(*l));
    if (!kv || !l) {
        free(kv);
        free(l);
        return NULL;
    }
    l->magic = KV_LSM_MAGIC;
    memcpy(l->dir, dir, dlen + 1);
    if (cfg)
        l->cfg = *cfg;
    l->next_seq = 1;
    l->apply_next = 1;
    /* Recursive: a scan callback is allowed to read the same store (
     * efs_meta_apply_check does exactly that), and the callback runs with
     * the structural state held. Writing from a callback is not supported. */
    if (pthread_mutexattr_init(&mattr) != 0) {
        free(kv);
        free(l);
        return NULL;
    }
    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    if (pthread_mutex_init(&l->mu, &mattr) != 0) {
        pthread_mutexattr_destroy(&mattr);
        free(kv);
        free(l);
        return NULL;
    }
    if (pthread_cond_init(&l->cv, NULL) != 0) {
        pthread_mutex_destroy(&l->mu);
        pthread_mutexattr_destroy(&mattr);
        free(kv);
        free(l);
        return NULL;
    }
    if (pthread_cond_init(&l->compact_cv, NULL) != 0) {
        pthread_cond_destroy(&l->cv);
        pthread_mutex_destroy(&l->mu);
        pthread_mutexattr_destroy(&mattr);
        free(kv);
        free(l);
        return NULL;
    }
    pthread_mutexattr_destroy(&mattr);
    if (manifest_load(l) != EFS_OK) {
        lsm_destroy(l);
        free(kv);
        return NULL;
    }
    gc_orphans(l);
    snprintf(wal_path, sizeof(wal_path), "%s/wal", l->dir);
    /* Replayed records rebuild the memtable and stay in the log: the reset
     * that discards them only runs once they are a durable segment. */
    if (kv_wal_replay(wal_path, replay_item, l) != EFS_OK) {
        lsm_destroy(l);
        free(kv);
        return NULL;
    }
    if (kv_wal_open(wal_path, l->cfg.sync_mode, &l->wal) != EFS_OK) {
        lsm_destroy(l);
        free(kv);
        return NULL;
    }
    kv->ops = &lsm_ops;
    kv->ctx = l;
    kv_compactor_start(l);
    return kv;
}

void efs_kv_lsm_close(struct efs_kv *kv)
{
    if (!kv)
        return;
    efs_kv_destroy(kv);
    free(kv);
}

static int lsm_flush(struct efs_kv *kv, int wait)
{
    struct kv_lsm *l;
    int rc;

    if (!kv || !kv->ctx)
        return EFS_ERR_INVAL;
    l = kv->ctx;
    pthread_mutex_lock(&l->mu);
    /* A flush emits at most one L0 file per key[0]. Counting those runs
     * walks the memtable. The raft pump calls this on every snapshot
     * open; refuse before the walk when a full memtable cannot fit. */
    if (!wait && l->mt.n > 0 && kv_l0_bytes(l) >= KV_LSM_L0_BYTES) {
        if (l->compact_started) {
            l->compact_req = 1;
            pthread_cond_signal(&l->compact_cv);
        }
        pthread_mutex_unlock(&l->mu);
        return EFS_ERR_BUSY;
    }
    /* Snapshot flush only needs the memtable durable. Compaction is the
     * background thread's job; at the engine cap, wait for it instead of
     * rewriting L1 on this call. The pump must not wait. */
    if (wait) {
        while (kv_l0_bytes(l) >= KV_LSM_L0_BYTES && !l->io_failed) {
            if (!l->compact_started) {
                rc = kv_compact_locked(l, 0);
                if (rc != EFS_OK) {
                    pthread_mutex_unlock(&l->mu);
                    return rc;
                }
                break;
            }
            l->compact_req = 1;
            pthread_cond_signal(&l->compact_cv);
            pthread_cond_wait(&l->cv, &l->mu);
        }
    }
    if (l->io_failed) {
        pthread_mutex_unlock(&l->mu);
        return EFS_ERR_IO;
    }
    rc = kv_flush_locked(l);
    if (rc == EFS_OK && l->compact_started) {
        uint32_t l0max = l->cfg.l0_max ? l->cfg.l0_max : KV_LSM_L0_DEFAULT;

        if (l->n_l0 >= l0max) {
            l->compact_req = 1;
            pthread_cond_signal(&l->compact_cv);
        }
    }
    pthread_mutex_unlock(&l->mu);
    return rc;
}

int efs_kv_lsm_flush(struct efs_kv *kv)
{
    return lsm_flush(kv, 1);
}

int efs_kv_lsm_flush_nowait(struct efs_kv *kv)
{
    return lsm_flush(kv, 0);
}

int efs_kv_lsm_compact(struct efs_kv *kv)
{
    struct kv_lsm *l;
    int rc;

    if (!kv || !kv->ctx)
        return EFS_ERR_INVAL;
    l = kv->ctx;
    pthread_mutex_lock(&l->mu);
    while (l->compact_busy)
        pthread_cond_wait(&l->cv, &l->mu);
    l->compact_busy = 1;
    rc = kv_compact_locked(l, 0);
    l->compact_busy = 0;
    pthread_cond_broadcast(&l->cv);
    pthread_mutex_unlock(&l->mu);
    return rc;
}

int efs_kv_lsm_l0_hot(struct efs_kv *kv)
{
    struct kv_lsm *l;
    int hot;

    if (!kv || !kv->ctx)
        return 0;
    l = kv->ctx;
    pthread_mutex_lock(&l->mu);
    hot = kv_l0_bytes(l) >= KV_LSM_L0_BYTES;
    pthread_mutex_unlock(&l->mu);
    return hot;
}

int efs_kv_lsm_seg_count(struct efs_kv *kv, uint32_t *l0, uint32_t *l1)
{
    struct kv_lsm *l;

    if (!kv || !kv->ctx)
        return EFS_ERR_INVAL;
    l = kv->ctx;
    pthread_mutex_lock(&l->mu);
    if (l0)
        *l0 = l->n_l0;
    if (l1)
        *l1 = l->n_l1;
    pthread_mutex_unlock(&l->mu);
    return EFS_OK;
}

int efs_kv_lsm_sync_hold(struct efs_kv *kv)
{
    struct kv_lsm *l;

    if (!kv || kv->ops != &lsm_ops || !kv->ctx)
        return EFS_OK;
    l = kv->ctx;
    if (l->magic != KV_LSM_MAGIC || !l->wal)
        return EFS_OK;
    return kv_wal_hold(l->wal, 1);
}

int efs_kv_lsm_sync_release(struct efs_kv *kv)
{
    struct kv_lsm *l;

    if (!kv || kv->ops != &lsm_ops || !kv->ctx)
        return EFS_OK;
    l = kv->ctx;
    if (l->magic != KV_LSM_MAGIC || !l->wal)
        return EFS_OK;
    return kv_wal_hold(l->wal, 0);
}

struct efs_kv_lsm_view {
    struct kv_seg **seg;
    uint32_t n;
};

int efs_kv_lsm_view_pin(struct efs_kv *kv, struct efs_kv_lsm_view **out)
{
    struct kv_lsm *l;
    struct efs_kv_lsm_view *v;
    uint32_t i;

    if (!kv || kv->ops != &lsm_ops || !kv->ctx || !out)
        return EFS_ERR_INVAL;
    l = kv->ctx;
    v = calloc(1, sizeof(*v));
    if (!v)
        return EFS_ERR_NOMEM;
    pthread_mutex_lock(&l->mu);
    v->seg = calloc(l->n_l0 + l->n_l1 ? l->n_l0 + l->n_l1 : 1,
                    sizeof(*v->seg));
    if (!v->seg) {
        pthread_mutex_unlock(&l->mu);
        free(v);
        return EFS_ERR_NOMEM;
    }
    for (i = 0; i < l->n_l0; i++) {
        v->seg[v->n] = l->l0[i].seg;
        kv_seg_pin(v->seg[v->n]);
        v->n++;
    }
    for (i = 0; i < l->n_l1; i++) {
        v->seg[v->n] = l->l1[i].seg;
        kv_seg_pin(v->seg[v->n]);
        v->n++;
    }
    pthread_mutex_unlock(&l->mu);
    *out = v;
    return EFS_OK;
}

int efs_kv_lsm_view_get(struct efs_kv_lsm_view *v, const uint8_t *key,
                        uint32_t klen, uint8_t *val, uint32_t *vlen)
{
    struct kv_buf b;
    uint32_t i;

    if (!v || !key || klen == 0 || !vlen)
        return EFS_ERR_INVAL;
    memset(&b, 0, sizeof(b));
    for (i = 0; i < v->n; i++) {
        uint8_t op = 0;
        int rc = kv_seg_get(v->seg[i], key, klen, &b, &op);

        if (rc == EFS_ERR_NOT_FOUND)
            continue;
        if (rc != EFS_OK) {
            kv_buf_free(&b);
            return rc;
        }
        if (op == KV_OP_DEL) {
            kv_buf_free(&b);
            return EFS_ERR_NOT_FOUND;
        }
        if (*vlen < b.len || (b.len > 0 && !val)) {
            *vlen = b.len;
            kv_buf_free(&b);
            return EFS_ERR_NOMEM;
        }
        if (b.len)
            memcpy(val, b.p, b.len);
        *vlen = b.len;
        kv_buf_free(&b);
        return EFS_OK;
    }
    kv_buf_free(&b);
    return EFS_ERR_NOT_FOUND;
}

void efs_kv_lsm_view_unpin(struct efs_kv_lsm_view *v)
{
    uint32_t i;

    if (!v)
        return;
    for (i = 0; i < v->n; i++)
        kv_seg_unpin(v->seg[i]);
    free(v->seg);
    free(v);
}

/* Group export of a pinned view. Private segment opens: the live block
 * cache is not shared with the apply path. The caller flushed the
 * memtable before the pin, so the view is the whole state. Newest
 * segment wins; tombstones are dropped. File format matches
 * efs_kv_group_import (little-endian nitems, then PUT items). */
static void vx_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* One write() per key was the top of efsd during posix: the export
 * walks the whole table on the GC thread, and each item was three
 * syscalls (header, key, value). 64 KiB absorbs those. */
#define VX_BUF (64u * 1024u)

struct vx_buf {
    int fd;
    uint8_t *p;
    size_t n;
};

static int vx_flush(struct vx_buf *b)
{
    size_t off = 0;

    while (off < b->n) {
        ssize_t w = write(b->fd, b->p + off, b->n - off);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            return EFS_ERR_IO;
        }
        if (w == 0)
            return EFS_ERR_IO;
        off += (size_t)w;
    }
    b->n = 0;
    return EFS_OK;
}

static int vx_write(struct vx_buf *b, const void *p, size_t n)
{
    const uint8_t *s = p;

    if (n >= VX_BUF) {
        int rc = vx_flush(b);

        if (rc != EFS_OK)
            return rc;
        while (n) {
            ssize_t w = write(b->fd, s, n);

            if (w < 0) {
                if (errno == EINTR)
                    continue;
                return EFS_ERR_IO;
            }
            if (w == 0)
                return EFS_ERR_IO;
            s += (size_t)w;
            n -= (size_t)w;
        }
        return EFS_OK;
    }
    if (b->n + n > VX_BUF) {
        int rc = vx_flush(b);

        if (rc != EFS_OK)
            return rc;
    }
    if (n)
        memcpy(b->p + b->n, s, n);
    b->n += n;
    return EFS_OK;
}

static int vx_in_group(const uint8_t *key, uint32_t klen, uint8_t group)
{
    uint32_t shard;

    if (klen < 2)
        return 0;
    shard = ((uint32_t)key[0] << 8) | (uint32_t)key[1];
    if (shard > EFS_KV_SHARD_MASK)
        return 0;
    return efs_raft_shard_group(shard) == group;
}

struct vx_src {
    struct kv_seg *seg;
    struct kv_seg_iter *it;
    const uint8_t *key;
    const uint8_t *val;
    uint32_t klen;
    uint32_t vlen;
    uint8_t op;
    int done;
};

static int vx_pull(struct vx_src *s)
{
    int rc = kv_seg_iter_next(s->it, &s->key, &s->klen, &s->val, &s->vlen,
                              &s->op);

    if (rc == EFS_ERR_NOT_FOUND) {
        s->done = 1;
        return EFS_OK;
    }
    return rc;
}

/* Min-heap of source indexes. Lowest index wins a tie: the view pins L0
 * newest-first, and the old linear scan used strict < so that source
 * supplied the value. A full scan per key was the cpu-clock top of efsd
 * during posix (memcmp inside this export, on the GC thread). */
static int vx_before(const struct vx_src *src, uint32_t a, uint32_t b)
{
    int c = kv_key_cmp(src[a].key, src[a].klen, src[b].key, src[b].klen);

    if (c != 0)
        return c < 0;
    return a < b;
}

static void vx_swap(uint32_t *h, uint32_t i, uint32_t j)
{
    uint32_t t = h[i];

    h[i] = h[j];
    h[j] = t;
}

static void vx_sift_up(uint32_t *h, const struct vx_src *src, uint32_t i)
{
    while (i > 0) {
        uint32_t p = (i - 1) / 2;

        if (!vx_before(src, h[i], h[p]))
            break;
        vx_swap(h, i, p);
        i = p;
    }
}

static void vx_sift_down(uint32_t *h, uint32_t n, const struct vx_src *src,
                         uint32_t i)
{
    for (;;) {
        uint32_t l = i * 2 + 1;
        uint32_t r = l + 1;
        uint32_t best = i;

        if (l < n && vx_before(src, h[l], h[best]))
            best = l;
        if (r < n && vx_before(src, h[r], h[best]))
            best = r;
        if (best == i)
            break;
        vx_swap(h, i, best);
        i = best;
    }
}

static void vx_push(uint32_t *h, uint32_t *n, const struct vx_src *src,
                    uint32_t idx)
{
    h[*n] = idx;
    vx_sift_up(h, src, *n);
    (*n)++;
}

static uint32_t vx_pop(uint32_t *h, uint32_t *n, const struct vx_src *src)
{
    uint32_t top = h[0];

    (*n)--;
    if (*n > 0) {
        h[0] = h[*n];
        vx_sift_down(h, *n, src, 0);
    }
    return top;
}

int efs_kv_lsm_view_export(struct efs_kv_lsm_view *v, uint8_t group,
                           const char *path)
{
    struct vx_src *src = NULL;
    uint32_t *heap = NULL;
    struct vx_buf outb;
    char tmp[4096];
    uint8_t *wkbuf;
    uint32_t nsrc = 0, i, nitems = 0, hn = 0, cap;
    int fd = -1, rc = EFS_OK;

    memset(&outb, 0, sizeof(outb));
    outb.fd = -1;
    wkbuf = malloc(KV_LSM_KLEN_MAX);
    if (!wkbuf)
        return EFS_ERR_NOMEM;

    if (!v || !path || (group != 0 && group != 2)) {
        free(wkbuf);
        return EFS_ERR_INVAL;
    }
    cap = v->n ? v->n : 1;
    src = calloc(cap, sizeof(*src));
    heap = calloc(cap, sizeof(*heap));
    if (!src || !heap) {
        free(src);
        free(heap);
        free(wkbuf);
        return EFS_ERR_NOMEM;
    }
    for (i = 0; i < v->n; i++) {
        const char *sp = kv_seg_filepath(v->seg[i]);

        if (!sp)
            continue;
        rc = kv_seg_open(sp, &src[nsrc].seg);
        if (rc != EFS_OK)
            goto out;
        rc = kv_seg_iter_open(src[nsrc].seg, &src[nsrc].it);
        if (rc != EFS_OK)
            goto out;
        kv_seg_iter_set_seq(src[nsrc].it, 1);
        rc = kv_seg_iter_seek(src[nsrc].it, NULL, 0);
        if (rc != EFS_OK)
            goto out;
        rc = vx_pull(&src[nsrc]);
        if (rc != EFS_OK)
            goto out;
        nsrc++;
    }
    if (strlen(path) + 5 >= sizeof(tmp)) {
        rc = EFS_ERR_INVAL;
        goto out;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        rc = EFS_ERR_IO;
        goto out;
    }
    outb.fd = fd;
    outb.p = malloc(VX_BUF);
    if (!outb.p) {
        rc = EFS_ERR_NOMEM;
        goto out;
    }
    {
        uint8_t z[4] = {0, 0, 0, 0};
        rc = vx_write(&outb, z, 4);
        if (rc != EFS_OK)
            goto out;
    }
    for (i = 0; i < nsrc; i++) {
        if (!src[i].done)
            vx_push(heap, &hn, src, i);
    }
    while (hn > 0) {
        uint32_t win = vx_pop(heap, &hn, src);
        uint8_t hdr[9];
        uint32_t wl;

        if (src[win].op == KV_OP_PUT &&
            vx_in_group(src[win].key, src[win].klen, group)) {
            if (nitems == 0xffffffffu) {
                rc = EFS_ERR_NOMEM;
                goto out;
            }
            hdr[0] = KV_OP_PUT;
            vx_put_u32(hdr + 1, src[win].klen);
            vx_put_u32(hdr + 5, src[win].vlen);
            rc = vx_write(&outb, hdr, 9);
            if (rc == EFS_OK && src[win].klen)
                rc = vx_write(&outb, src[win].key, src[win].klen);
            if (rc == EFS_OK && src[win].vlen)
                rc = vx_write(&outb, src[win].val, src[win].vlen);
            if (rc != EFS_OK)
                goto out;
            nitems++;
        }
        wl = src[win].klen;
        if (wl > KV_LSM_KLEN_MAX) {
            rc = EFS_ERR_INVAL;
            goto out;
        }
        if (wl)
            memcpy(wkbuf, src[win].key, wl);
        /* Advance the winner after the other sources still sitting on
         * this key. Pushing it first would hide a second copy of the
         * same key inside that one segment. */
        rc = vx_pull(&src[win]);
        if (rc != EFS_OK)
            goto out;
        while (hn > 0 &&
               kv_key_cmp(src[heap[0]].key, src[heap[0]].klen, wkbuf, wl) == 0) {
            uint32_t j = vx_pop(heap, &hn, src);

            rc = vx_pull(&src[j]);
            if (rc != EFS_OK)
                goto out;
            if (!src[j].done)
                vx_push(heap, &hn, src, j);
        }
        if (!src[win].done)
            vx_push(heap, &hn, src, win);
    }
    /* Flush before the seek. A buffered tail written after lseek(0)
     * would overwrite the item count. */
    rc = vx_flush(&outb);
    if (rc != EFS_OK)
        goto out;
    if (lseek(fd, 0, SEEK_SET) < 0) {
        rc = EFS_ERR_IO;
        goto out;
    }
    {
        uint8_t nb[4];
        vx_put_u32(nb, nitems);
        rc = vx_write(&outb, nb, 4);
        if (rc == EFS_OK)
            rc = vx_flush(&outb);
        if (rc != EFS_OK)
            goto out;
    }
    if (fsync(fd) != 0) {
        rc = EFS_ERR_IO;
        goto out;
    }
    if (close(fd) != 0) {
        fd = -1;
        rc = EFS_ERR_IO;
        goto out;
    }
    fd = -1;
    if (rename(tmp, path) != 0) {
        rc = EFS_ERR_IO;
        goto out;
    }
    {
        char dir[4096];
        char *slash;

        snprintf(dir, sizeof(dir), "%s", path);
        slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            kv_sync_dir(dir);
        }
    }
    rc = EFS_OK;
out:
    free(outb.p);
    free(wkbuf);
    if (fd >= 0)
        close(fd);
    if (rc != EFS_OK)
        unlink(tmp);
    for (i = 0; i < nsrc; i++) {
        kv_seg_iter_close(src[i].it);
        kv_seg_close(src[i].seg);
    }
    free(src);
    free(heap);
    return rc;
}

int efs_kv_lsm_quiesce(struct efs_kv *kv)
{
    struct kv_lsm *l;
    uint32_t l0max;

    if (!kv || kv->ops != &lsm_ops || !kv->ctx)
        return EFS_ERR_INVAL;
    l = kv->ctx;
    pthread_mutex_lock(&l->mu);
    l0max = l->cfg.l0_max ? l->cfg.l0_max : KV_LSM_L0_DEFAULT;
    while (!l->io_failed && (l->compact_busy || l->n_l0 >= l0max)) {
        if (!l->compact_started) {
            int rc = kv_compact_locked(l, 0);

            pthread_mutex_unlock(&l->mu);
            return rc;
        }
        l->compact_req = 1;
        pthread_cond_signal(&l->compact_cv);
        pthread_cond_wait(&l->cv, &l->mu);
    }
    pthread_mutex_unlock(&l->mu);
    return l->io_failed ? EFS_ERR_IO : EFS_OK;
}
