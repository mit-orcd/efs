#include "kv_lsm_internal.h"

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

static void ent_free(struct kv_ent *e)
{
    if (!e)
        return;
    free(e->key);
    free(e->val);
    free(e);
}

void kv_mtab_clear(struct kv_mtab *m)
{
    uint32_t i;

    for (i = 0; i < m->n; i++)
        ent_free(m->e[i]);
    free(m->e);
    m->e = NULL;
    m->n = 0;
    m->cap = 0;
    m->bytes = 0;
}

/* Index of key, or the insertion point with *found = 0. */
uint32_t kv_mtab_pos(const struct kv_mtab *m, const uint8_t *key,
                         uint32_t klen, int *found)
{
    uint32_t lo = 0, hi = m->n;

    *found = 0;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = kv_key_cmp(m->e[mid]->key, m->e[mid]->klen, key, klen);
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
        if (!p)
            return EFS_ERR_NOMEM;
        m->e = p;
        m->cap = cap;
    }
    e = calloc(1, sizeof(*e));
    if (!e)
        return EFS_ERR_NOMEM;
    e->key = malloc(klen);
    if (!e->key) {
        free(e);
        return EFS_ERR_NOMEM;
    }
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
    if (at < m->n)
        memmove(&m->e[at + 1], &m->e[at], (m->n - at) * sizeof(*m->e));
    m->e[at] = e;
    m->n++;
    m->bytes += klen + vlen + sizeof(*e) + sizeof(e);
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
        if ((level != 0 && level != 1) ||
            (level == 0 ? l->n_l0 : l->n_l1) >= KV_LSM_MAX_SEGS) {
            rc = EFS_ERR_PROTO;
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

/* EFS_OK with *op, or NOT_FOUND when no level speaks about the key. */
static int lookup(struct kv_lsm *l, const uint8_t *key, uint32_t klen,
                  struct kv_buf *out, uint8_t *op)
{
    uint32_t i;
    int found;
    uint32_t at = kv_mtab_pos(&l->mt, key, klen, &found);

    if (found) {
        struct kv_ent *e = l->mt.e[at];
        *op = e->op;
        return kv_buf_set(out, e->val, e->vlen);
    }
    for (i = 0; i < l->n_l0; i++) {
        int rc = kv_seg_get(l->l0[i].seg, key, klen, out, op);
        if (rc != EFS_ERR_NOT_FOUND)
            return rc;
    }
    for (i = 0; i < l->n_l1; i++) {
        int rc = kv_seg_get(l->l1[i].seg, key, klen, out, op);
        if (rc != EFS_ERR_NOT_FOUND)
            return rc;
    }
    return EFS_ERR_NOT_FOUND;
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
    struct msrc src[KV_LSM_MAX_SEGS * 2 + 1];
    const uint8_t *seek = slen ? start : prefix;
    uint32_t seek_len = slen ? slen : plen;
    uint32_t nsrc = 0, i;
    int rc = EFS_OK;

    memset(src, 0, sizeof(src));
    src[nsrc].mi = 0;
    if (l->mt.n && seek_len) {
        int found;
        src[nsrc].mi = kv_mtab_pos(&l->mt, seek, seek_len, &found);
    }
    nsrc++;
    for (i = 0; i < l->n_l0 + l->n_l1; i++) {
        struct seg_slot *sl = i < l->n_l0 ? &l->l0[i] : &l->l1[i - l->n_l0];
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
    /* Durable first: a batch is invisible until its record is fsynced, so a
     * failure leaves no trace. Concurrent batches share that fsync. */
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
    kv_wal_close(l->wal);
    drop_segs(l);
    kv_mtab_clear(&l->mt);
    kv_buf_free(&l->scratch);
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
    if (pthread_mutex_init(&l->mu, &mattr) != 0 ||
        pthread_cond_init(&l->cv, NULL) != 0) {
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
    return kv;
}

void efs_kv_lsm_close(struct efs_kv *kv)
{
    if (!kv)
        return;
    efs_kv_destroy(kv);
    free(kv);
}

int efs_kv_lsm_flush(struct efs_kv *kv)
{
    struct kv_lsm *l;
    int rc;

    if (!kv || !kv->ctx)
        return EFS_ERR_INVAL;
    l = kv->ctx;
    pthread_mutex_lock(&l->mu);
    rc = kv_flush_locked(l);
    pthread_mutex_unlock(&l->mu);
    return rc;
}

int efs_kv_lsm_compact(struct efs_kv *kv)
{
    struct kv_lsm *l;
    int rc;

    if (!kv || !kv->ctx)
        return EFS_ERR_INVAL;
    l = kv->ctx;
    pthread_mutex_lock(&l->mu);
    rc = kv_compact_locked(l);
    pthread_mutex_unlock(&l->mu);
    return rc;
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
