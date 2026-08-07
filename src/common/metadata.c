#include "efs/metadata.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#define EFS_META_MAGIC "EFSM"
#define EFS_META_ROOT_MAGIC "EFSR"
/* v2: uid/gid. v3: mtime_nsec after mtime. v4: atime + dir rollups. */
#define EFS_META_VERSION 4
#define EFS_META_ROOT_VERSION 1

static void time_now(uint64_t *t)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *t = (uint64_t)ts.tv_sec;
}

static uint64_t hash_mix(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x ? x : 1;
}

static uint64_t hash_name_key(efs_ino_t parent, const char *name)
{
    uint64_t h = hash_mix(parent);
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = h * 131u + *p;
    return h ? h : 1;
}

static uint64_t hash_chunk_key(efs_ino_t ino, uint32_t chunk_index)
{
    return hash_mix(ino ^ ((uint64_t)chunk_index * 0x9E3779B97F4A7C15ULL));
}

static int idx_init(uint64_t **keys, uint64_t **vals, uint64_t *mask, uint64_t n_hint)
{
    uint64_t cap = 16;
    while (cap < n_hint * 2)
        cap *= 2;
    free(*keys);
    free(*vals);
    *keys = calloc(cap, sizeof(uint64_t));
    *vals = calloc(cap, sizeof(uint64_t));
    if (!*keys || !*vals) {
        free(*keys);
        free(*vals);
        *keys = *vals = NULL;
        *mask = 0;
        return -1;
    }
    *mask = cap - 1;
    return 0;
}

static void idx_free(uint64_t **keys, uint64_t **vals, uint64_t *mask)
{
    free(*keys);
    free(*vals);
    *keys = *vals = NULL;
    *mask = 0;
}

static int idx_put(uint64_t *keys, uint64_t *vals, uint64_t mask, uint64_t key, uint64_t val)
{
    if (!keys || mask == 0)
        return -1;
    if (key == 0)
        key = 1;
    uint64_t i = key & mask;
    for (uint64_t n = 0; n <= mask; n++) {
        if (keys[i] == 0 || keys[i] == key) {
            keys[i] = key;
            vals[i] = val;
            return 0;
        }
        i = (i + 1) & mask;
    }
    return -1;
}

static int idx_get(const uint64_t *keys, const uint64_t *vals, uint64_t mask,
                   uint64_t key, uint64_t *val)
{
    if (!keys || mask == 0)
        return -1;
    if (key == 0)
        key = 1;
    uint64_t i = key & mask;
    for (uint64_t n = 0; n <= mask; n++) {
        if (keys[i] == 0)
            return -1;
        if (keys[i] == key) {
            *val = vals[i];
            return 0;
        }
        i = (i + 1) & mask;
    }
    return -1;
}

/* Tombstone-free delete: remove key and re-insert the probe chain after it. */
static void idx_del(uint64_t *keys, uint64_t *vals, uint64_t mask, uint64_t key)
{
    if (!keys || mask == 0)
        return;
    if (key == 0)
        key = 1;
    uint64_t i = key & mask;
    for (uint64_t n = 0; n <= mask; n++) {
        if (keys[i] == 0)
            return;
        if (keys[i] == key)
            break;
        i = (i + 1) & mask;
    }
    if (keys[i] != key)
        return;
    keys[i] = 0;
    vals[i] = 0;
    uint64_t j = (i + 1) & mask;
    while (keys[j] != 0) {
        uint64_t k = keys[j];
        uint64_t v = vals[j];
        keys[j] = 0;
        vals[j] = 0;
        idx_put(keys, vals, mask, k, v);
        j = (j + 1) & mask;
    }
}

/* Name/chunk indexes may share a hash across distinct keys; probe past
 * hash matches that fail (parent,name) / (ino,chunk_index) verification. */
static int name_idx_put(struct efs_export *ex, efs_ino_t parent, const char *name,
                        uint64_t pos)
{
    if (!ex->name_keys || ex->name_mask == 0)
        return -1;
    uint64_t key = hash_name_key(parent, name);
    uint64_t i = key & ex->name_mask;
    for (uint64_t n = 0; n <= ex->name_mask; n++) {
        if (ex->name_keys[i] == 0) {
            ex->name_keys[i] = key;
            ex->name_vals[i] = pos;
            return 0;
        }
        if (ex->name_keys[i] == key) {
            uint64_t p = ex->name_vals[i];
            if (p < ex->inode_count &&
                ex->inodes[p].parent == parent &&
                strcmp(ex->inodes[p].name, name) == 0) {
                ex->name_vals[i] = pos;
                return 0;
            }
        }
        i = (i + 1) & ex->name_mask;
    }
    return -1;
}

static int name_idx_get(struct efs_export *ex, efs_ino_t parent, const char *name,
                        uint64_t *pos)
{
    if (!ex->name_keys || ex->name_mask == 0)
        return -1;
    uint64_t key = hash_name_key(parent, name);
    uint64_t i = key & ex->name_mask;
    for (uint64_t n = 0; n <= ex->name_mask; n++) {
        if (ex->name_keys[i] == 0)
            return -1;
        if (ex->name_keys[i] == key) {
            uint64_t p = ex->name_vals[i];
            if (p < ex->inode_count &&
                ex->inodes[p].parent == parent &&
                strcmp(ex->inodes[p].name, name) == 0) {
                *pos = p;
                return 0;
            }
        }
        i = (i + 1) & ex->name_mask;
    }
    return -1;
}

static void name_idx_del(struct efs_export *ex, efs_ino_t parent, const char *name)
{
    if (!ex->name_keys || ex->name_mask == 0)
        return;
    uint64_t key = hash_name_key(parent, name);
    uint64_t i = key & ex->name_mask;
    for (uint64_t n = 0; n <= ex->name_mask; n++) {
        if (ex->name_keys[i] == 0)
            return;
        if (ex->name_keys[i] == key) {
            uint64_t p = ex->name_vals[i];
            if (p < ex->inode_count &&
                ex->inodes[p].parent == parent &&
                strcmp(ex->inodes[p].name, name) == 0)
                break;
        }
        i = (i + 1) & ex->name_mask;
    }
    if (ex->name_keys[i] != key)
        return;
    /* Confirm identity again in case the loop exited on capacity. */
    {
        uint64_t p = ex->name_vals[i];
        if (!(p < ex->inode_count &&
              ex->inodes[p].parent == parent &&
              strcmp(ex->inodes[p].name, name) == 0))
            return;
    }
    ex->name_keys[i] = 0;
    ex->name_vals[i] = 0;
    uint64_t j = (i + 1) & ex->name_mask;
    while (ex->name_keys[j] != 0) {
        uint64_t k = ex->name_keys[j];
        uint64_t v = ex->name_vals[j];
        ex->name_keys[j] = 0;
        ex->name_vals[j] = 0;
        if (v < ex->inode_count)
            name_idx_put(ex, ex->inodes[v].parent, ex->inodes[v].name, v);
        else
            idx_put(ex->name_keys, ex->name_vals, ex->name_mask, k, v);
        j = (j + 1) & ex->name_mask;
    }
}

static int chunk_idx_put(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         uint64_t pos)
{
    if (!ex->chunk_keys || ex->chunk_mask == 0)
        return -1;
    uint64_t key = hash_chunk_key(ino, chunk_index);
    uint64_t i = key & ex->chunk_mask;
    for (uint64_t n = 0; n <= ex->chunk_mask; n++) {
        if (ex->chunk_keys[i] == 0) {
            ex->chunk_keys[i] = key;
            ex->chunk_vals[i] = pos;
            return 0;
        }
        if (ex->chunk_keys[i] == key) {
            uint64_t p = ex->chunk_vals[i];
            if (p < ex->chunk_count &&
                ex->chunks[p].ino == ino &&
                ex->chunks[p].chunk_index == chunk_index) {
                ex->chunk_vals[i] = pos;
                return 0;
            }
        }
        i = (i + 1) & ex->chunk_mask;
    }
    return -1;
}

static int chunk_idx_get(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         uint64_t *pos)
{
    if (!ex->chunk_keys || ex->chunk_mask == 0)
        return -1;
    uint64_t key = hash_chunk_key(ino, chunk_index);
    uint64_t i = key & ex->chunk_mask;
    for (uint64_t n = 0; n <= ex->chunk_mask; n++) {
        if (ex->chunk_keys[i] == 0)
            return -1;
        if (ex->chunk_keys[i] == key) {
            uint64_t p = ex->chunk_vals[i];
            if (p < ex->chunk_count &&
                ex->chunks[p].ino == ino &&
                ex->chunks[p].chunk_index == chunk_index) {
                *pos = p;
                return 0;
            }
        }
        i = (i + 1) & ex->chunk_mask;
    }
    return -1;
}

static void chunk_idx_del(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index)
{
    if (!ex->chunk_keys || ex->chunk_mask == 0)
        return;
    uint64_t key = hash_chunk_key(ino, chunk_index);
    uint64_t i = key & ex->chunk_mask;
    for (uint64_t n = 0; n <= ex->chunk_mask; n++) {
        if (ex->chunk_keys[i] == 0)
            return;
        if (ex->chunk_keys[i] == key) {
            uint64_t p = ex->chunk_vals[i];
            if (p < ex->chunk_count &&
                ex->chunks[p].ino == ino &&
                ex->chunks[p].chunk_index == chunk_index)
                break;
        }
        i = (i + 1) & ex->chunk_mask;
    }
    if (ex->chunk_keys[i] != key)
        return;
    {
        uint64_t p = ex->chunk_vals[i];
        if (!(p < ex->chunk_count &&
              ex->chunks[p].ino == ino &&
              ex->chunks[p].chunk_index == chunk_index))
            return;
    }
    ex->chunk_keys[i] = 0;
    ex->chunk_vals[i] = 0;
    uint64_t j = (i + 1) & ex->chunk_mask;
    while (ex->chunk_keys[j] != 0) {
        uint64_t v = ex->chunk_vals[j];
        ex->chunk_keys[j] = 0;
        ex->chunk_vals[j] = 0;
        if (v < ex->chunk_count)
            chunk_idx_put(ex, ex->chunks[v].ino, ex->chunks[v].chunk_index, v);
        j = (j + 1) & ex->chunk_mask;
    }
}

static int export_reindex(struct efs_export *ex)
{
    if (idx_init(&ex->ino_keys, &ex->ino_vals, &ex->ino_mask,
                 ex->inode_count ? ex->inode_count : 16) != 0)
        return -1;
    if (idx_init(&ex->name_keys, &ex->name_vals, &ex->name_mask,
                 ex->inode_count ? ex->inode_count : 16) != 0)
        return -1;
    if (idx_init(&ex->chunk_keys, &ex->chunk_vals, &ex->chunk_mask,
                 ex->chunk_count ? ex->chunk_count : 16) != 0)
        return -1;
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, ex->inodes[i].ino, i);
        name_idx_put(ex, ex->inodes[i].parent, ex->inodes[i].name, i);
    }
    for (uint64_t i = 0; i < ex->chunk_count; i++) {
        chunk_idx_put(ex, ex->chunks[i].ino, ex->chunks[i].chunk_index, i);
    }
    return 0;
}

static int export_ensure_inode_idx(struct efs_export *ex)
{
    if (ex->ino_keys && ex->inode_count * 2 <= ex->ino_mask + 1)
        return 0;
    return export_reindex(ex);
}

static int export_ensure_chunk_idx(struct efs_export *ex)
{
    if (ex->chunk_keys && ex->chunk_count * 2 <= ex->chunk_mask + 1)
        return 0;
    return export_reindex(ex);
}

static struct efs_inode *inode_ptr(struct efs_export *ex, efs_ino_t ino)
{
    uint64_t pos = 0;
    if (ex->ino_keys) {
        /* Index is authoritative: a miss must not fall back to an O(n)
         * scan — every create/merge "is this ino free?" check would
         * otherwise re-walk the whole table. */
        if (idx_get(ex->ino_keys, ex->ino_vals, ex->ino_mask, ino, &pos) == 0 &&
            pos < ex->inode_count && ex->inodes[pos].ino == ino)
            return &ex->inodes[pos];
        return NULL;
    }
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (ex->inodes[i].ino == ino)
            return &ex->inodes[i];
    }
    return NULL;
}

/* ---- parent → children index (in-memory) ---- */

static void child_vecs_free(struct efs_export *ex)
{
    if (!ex)
        return;
    for (uint64_t i = 0; i < ex->child_vec_count; i++)
        free(ex->child_vecs[i].slots);
    free(ex->child_vecs);
    ex->child_vecs = NULL;
    ex->child_vec_count = 0;
    ex->child_vec_cap = 0;
    idx_free(&ex->child_keys, &ex->child_vals, &ex->child_mask);
}

static struct efs_child_vec *child_vec_get(struct efs_export *ex, efs_ino_t parent,
                                          int create)
{
    if (!ex->child_keys || ex->child_mask == 0) {
        if (!create)
            return NULL;
        uint64_t hint = ex->inode_count ? ex->inode_count : 16;
        if (idx_init(&ex->child_keys, &ex->child_vals, &ex->child_mask, hint) != 0)
            return NULL;
    }
    uint64_t vi = 0;
    if (idx_get(ex->child_keys, ex->child_vals, ex->child_mask, parent, &vi) == 0 &&
        vi < ex->child_vec_count)
        return &ex->child_vecs[vi];
    if (!create)
        return NULL;
    if (ex->child_vec_count >= ex->child_vec_cap) {
        uint64_t ncap = ex->child_vec_cap ? ex->child_vec_cap * 2 : 16;
        struct efs_child_vec *n = realloc(ex->child_vecs,
                                          ncap * sizeof(struct efs_child_vec));
        if (!n)
            return NULL;
        ex->child_vecs = n;
        ex->child_vec_cap = ncap;
    }
    /* Grow open-addressing table if load is high.
     * idx_init() frees *keys/*vals — detach old pointers first so we can
     * rehash from them, then free once (avoid double-free). */
    if (ex->child_vec_count * 2 > ex->child_mask) {
        uint64_t old_mask = ex->child_mask;
        uint64_t *ok = ex->child_keys;
        uint64_t *ov = ex->child_vals;
        ex->child_keys = NULL;
        ex->child_vals = NULL;
        ex->child_mask = 0;
        if (idx_init(&ex->child_keys, &ex->child_vals, &ex->child_mask,
                     ex->child_vec_count + 1) != 0) {
            ex->child_keys = ok;
            ex->child_vals = ov;
            ex->child_mask = old_mask;
            return NULL;
        }
        for (uint64_t i = 0; i <= old_mask; i++) {
            if (ok[i])
                idx_put(ex->child_keys, ex->child_vals, ex->child_mask, ok[i], ov[i]);
        }
        free(ok);
        free(ov);
    }
    vi = ex->child_vec_count++;
    memset(&ex->child_vecs[vi], 0, sizeof(ex->child_vecs[vi]));
    idx_put(ex->child_keys, ex->child_vals, ex->child_mask, parent, vi);
    return &ex->child_vecs[vi];
}

static int child_idx_add(struct efs_export *ex, efs_ino_t parent, uint64_t slot)
{
    if (parent == ex->inodes[slot].ino && parent == EFS_ROOT_INO)
        return 0; /* root is not a child of itself for listing */
    struct efs_child_vec *v = child_vec_get(ex, parent, 1);
    if (!v)
        return -1;
    if (v->count >= v->cap) {
        uint64_t ncap = v->cap ? v->cap * 2 : 4;
        uint64_t *ns = realloc(v->slots, ncap * sizeof(uint64_t));
        if (!ns)
            return -1;
        v->slots = ns;
        v->cap = ncap;
    }
    v->slots[v->count++] = slot;
    return 0;
}

static void child_idx_del(struct efs_export *ex, efs_ino_t parent, uint64_t slot)
{
    struct efs_child_vec *v = child_vec_get(ex, parent, 0);
    if (!v)
        return;
    for (uint64_t i = 0; i < v->count; i++) {
        if (v->slots[i] == slot) {
            v->slots[i] = v->slots[v->count - 1];
            v->count--;
            return;
        }
    }
}

static void child_idx_replace_slot(struct efs_export *ex, efs_ino_t parent,
                                   uint64_t old_slot, uint64_t new_slot)
{
    struct efs_child_vec *v = child_vec_get(ex, parent, 0);
    if (!v)
        return;
    for (uint64_t i = 0; i < v->count; i++) {
        if (v->slots[i] == old_slot) {
            v->slots[i] = new_slot;
            return;
        }
    }
}

static void child_idx_rebuild(struct efs_export *ex)
{
    child_vecs_free(ex);
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (ex->inodes[i].ino == EFS_ROOT_INO &&
            ex->inodes[i].parent == EFS_ROOT_INO)
            continue;
        child_idx_add(ex, ex->inodes[i].parent, i);
    }
}

int efs_export_foreach_child(struct efs_export *ex, efs_ino_t parent,
                             efs_child_cb cb, void *arg)
{
    if (!ex || !cb)
        return EFS_ERR_INVAL;
    struct efs_child_vec *v = child_vec_get(ex, parent, 0);
    if (!v)
        return EFS_OK;
    for (uint64_t i = 0; i < v->count; i++) {
        uint64_t slot = v->slots[i];
        if (slot >= ex->inode_count)
            continue;
        if (ex->inodes[slot].ino == parent)
            continue;
        int rc = cb(ex, slot, arg);
        if (rc != 0)
            return rc;
    }
    return EFS_OK;
}

/* ---- directory rollups ---- */

static uint64_t entry_tmin(const struct efs_inode *e)
{
    uint64_t t = e->mtime;
    if (e->ctime < t)
        t = e->ctime;
    if (e->atime < t)
        t = e->atime;
    return t;
}

static uint64_t entry_tmax(const struct efs_inode *e)
{
    uint64_t t = e->mtime;
    if (e->ctime > t)
        t = e->ctime;
    if (e->atime > t)
        t = e->atime;
    return t;
}

static void inode_clear_rollups(struct efs_inode *d)
{
    d->imm_files = d->imm_dirs = d->tree_files = d->tree_dirs = 0;
    d->imm_bytes = d->tree_bytes = 0;
    d->imm_tmin = d->imm_tmax = d->tree_tmin = d->tree_tmax = 0;
}

static void times_expand(uint64_t *tmin, uint64_t *tmax, uint64_t lo, uint64_t hi,
                         int *has)
{
    if (!*has) {
        *tmin = lo;
        *tmax = hi;
        *has = 1;
        return;
    }
    if (lo < *tmin)
        *tmin = lo;
    if (hi > *tmax)
        *tmax = hi;
}

/* Child C's contribution to a parent's tree_* aggregates. */
static void child_tree_contrib(const struct efs_inode *c, uint64_t *files,
                               uint64_t *dirs, uint64_t *bytes, uint64_t *tlo,
                               uint64_t *thi)
{
    *tlo = entry_tmin(c);
    *thi = entry_tmax(c);
    if (efs_mode_is_dir(c->mode)) {
        *files = c->tree_files;
        *dirs = 1 + c->tree_dirs;
        *bytes = c->tree_bytes;
        if (c->tree_files + c->tree_dirs > 0) {
            if (c->tree_tmin < *tlo)
                *tlo = c->tree_tmin;
            if (c->tree_tmax > *thi)
                *thi = c->tree_tmax;
        }
    } else {
        *files = 1;
        *dirs = 0;
        *bytes = c->size;
    }
}

static void parent_add_child(struct efs_inode *p, const struct efs_inode *c)
{
    uint64_t lo = entry_tmin(c), hi = entry_tmax(c);
    int has_imm = (p->imm_files + p->imm_dirs) > 0;
    int has_tree = (p->tree_files + p->tree_dirs) > 0;

    if (efs_mode_is_dir(c->mode)) {
        p->imm_dirs++;
        times_expand(&p->imm_tmin, &p->imm_tmax, lo, hi, &has_imm);

        uint64_t tf, td, tb, tlo, thi;
        child_tree_contrib(c, &tf, &td, &tb, &tlo, &thi);
        p->tree_files += tf;
        p->tree_dirs += td;
        p->tree_bytes += tb;
        times_expand(&p->tree_tmin, &p->tree_tmax, tlo, thi, &has_tree);
    } else {
        p->imm_files++;
        p->tree_files++;
        p->imm_bytes += c->size;
        p->tree_bytes += c->size;
        times_expand(&p->imm_tmin, &p->imm_tmax, lo, hi, &has_imm);
        times_expand(&p->tree_tmin, &p->tree_tmax, lo, hi, &has_tree);
    }
}

static void ancestor_add_tree(struct efs_inode *a, uint64_t files, uint64_t dirs,
                              uint64_t bytes, uint64_t tlo, uint64_t thi)
{
    int has = (a->tree_files + a->tree_dirs) > 0;
    a->tree_files += files;
    a->tree_dirs += dirs;
    a->tree_bytes += bytes;
    times_expand(&a->tree_tmin, &a->tree_tmax, tlo, thi, &has);
}

static void ancestor_sub_tree(struct efs_inode *a, uint64_t files, uint64_t dirs,
                              uint64_t bytes)
{
    if (a->tree_files >= files)
        a->tree_files -= files;
    else
        a->tree_files = 0;
    if (a->tree_dirs >= dirs)
        a->tree_dirs -= dirs;
    else
        a->tree_dirs = 0;
    if (a->tree_bytes >= bytes)
        a->tree_bytes -= bytes;
    else
        a->tree_bytes = 0;
}

static void parent_sub_child_counts(struct efs_inode *p, const struct efs_inode *c)
{
    if (efs_mode_is_dir(c->mode)) {
        if (p->imm_dirs > 0)
            p->imm_dirs--;
        uint64_t tf, td, tb, tlo, thi;
        child_tree_contrib(c, &tf, &td, &tb, &tlo, &thi);
        ancestor_sub_tree(p, tf, td, tb);
    } else {
        if (p->imm_files > 0)
            p->imm_files--;
        if (p->tree_files > 0)
            p->tree_files--;
        if (p->imm_bytes >= c->size)
            p->imm_bytes -= c->size;
        else
            p->imm_bytes = 0;
        if (p->tree_bytes >= c->size)
            p->tree_bytes -= c->size;
        else
            p->tree_bytes = 0;
    }
    if (p->imm_files + p->imm_dirs == 0) {
        p->imm_tmin = p->imm_tmax = 0;
        p->imm_bytes = 0;
    }
    if (p->tree_files + p->tree_dirs == 0) {
        p->tree_tmin = p->tree_tmax = 0;
        p->tree_bytes = 0;
    }
}

static void recompute_times_one(struct efs_export *ex, efs_ino_t dir_ino)
{
    struct efs_inode *d = inode_ptr(ex, dir_ino);
    if (!d || !efs_mode_is_dir(d->mode))
        return;
    d->imm_tmin = d->imm_tmax = d->tree_tmin = d->tree_tmax = 0;
    int has_imm = 0, has_tree = 0;
    struct efs_child_vec *v = child_vec_get(ex, dir_ino, 0);
    if (!v)
        return;
    for (uint64_t i = 0; i < v->count; i++) {
        uint64_t slot = v->slots[i];
        if (slot >= ex->inode_count)
            continue;
        struct efs_inode *c = &ex->inodes[slot];
        if (c->ino == dir_ino)
            continue;
        uint64_t lo = entry_tmin(c), hi = entry_tmax(c);
        times_expand(&d->imm_tmin, &d->imm_tmax, lo, hi, &has_imm);
        uint64_t tf, td, tb, tlo, thi;
        child_tree_contrib(c, &tf, &td, &tb, &tlo, &thi);
        (void)tf;
        (void)td;
        (void)tb;
        times_expand(&d->tree_tmin, &d->tree_tmax, tlo, thi, &has_tree);
    }
}

static void recompute_times_up(struct efs_export *ex, efs_ino_t dir_ino)
{
    efs_ino_t cur = dir_ino;
    for (;;) {
        recompute_times_one(ex, cur);
        struct efs_inode *d = inode_ptr(ex, cur);
        if (!d || d->parent == d->ino)
            break;
        cur = d->parent;
    }
}

static void rollup_add_under(struct efs_export *ex, efs_ino_t parent,
                             const struct efs_inode *child)
{
    struct efs_inode *p = inode_ptr(ex, parent);
    if (!p || !efs_mode_is_dir(p->mode))
        return;
    parent_add_child(p, child);

    uint64_t tf, td, tb, tlo, thi;
    child_tree_contrib(child, &tf, &td, &tb, &tlo, &thi);
    efs_ino_t a = p->parent;
    efs_ino_t prev = parent;
    while (a != prev) {
        struct efs_inode *ap = inode_ptr(ex, a);
        if (!ap || !efs_mode_is_dir(ap->mode))
            break;
        ancestor_add_tree(ap, tf, td, tb, tlo, thi);
        if (ap->parent == a)
            break;
        prev = a;
        a = ap->parent;
    }
}

static void rollup_sub_under(struct efs_export *ex, efs_ino_t parent,
                             const struct efs_inode *child)
{
    struct efs_inode *p = inode_ptr(ex, parent);
    if (!p || !efs_mode_is_dir(p->mode))
        return;

    uint64_t tf, td, tb, tlo, thi;
    child_tree_contrib(child, &tf, &td, &tb, &tlo, &thi);
    parent_sub_child_counts(p, child);

    efs_ino_t a = p->parent;
    efs_ino_t prev = parent;
    while (a != prev) {
        struct efs_inode *ap = inode_ptr(ex, a);
        if (!ap || !efs_mode_is_dir(ap->mode))
            break;
        ancestor_sub_tree(ap, tf, td, tb);
        if (ap->parent == a)
            break;
        prev = a;
        a = ap->parent;
    }
    recompute_times_up(ex, parent);
}

static void rollup_size_delta(struct efs_export *ex, efs_ino_t parent,
                              int64_t delta)
{
    if (delta == 0)
        return;
    efs_ino_t cur = parent;
    efs_ino_t prev = 0;
    int first = 1;
    while (cur != prev) {
        struct efs_inode *d = inode_ptr(ex, cur);
        if (!d || !efs_mode_is_dir(d->mode))
            break;
        if (first) {
            if (delta > 0)
                d->imm_bytes += (uint64_t)delta;
            else if (d->imm_bytes >= (uint64_t)(-delta))
                d->imm_bytes -= (uint64_t)(-delta);
            else
                d->imm_bytes = 0;
            first = 0;
        }
        if (delta > 0)
            d->tree_bytes += (uint64_t)delta;
        else if (d->tree_bytes >= (uint64_t)(-delta))
            d->tree_bytes -= (uint64_t)(-delta);
        else
            d->tree_bytes = 0;
        if (d->parent == cur)
            break;
        prev = cur;
        cur = d->parent;
    }
}

static void rollup_touch_parents_of(struct efs_export *ex, efs_ino_t ino)
{
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (ex->inodes[i].ino != ino)
            continue;
        recompute_times_up(ex, ex->inodes[i].parent);
    }
}

static void recompute_dir_postorder(struct efs_export *ex, efs_ino_t dir_ino)
{
    struct efs_inode *d = inode_ptr(ex, dir_ino);
    if (!d || !efs_mode_is_dir(d->mode))
        return;
    inode_clear_rollups(d);
    struct efs_child_vec *v = child_vec_get(ex, dir_ino, 0);
    if (!v)
        return;
    for (uint64_t i = 0; i < v->count; i++) {
        uint64_t slot = v->slots[i];
        if (slot >= ex->inode_count)
            continue;
        struct efs_inode *c = &ex->inodes[slot];
        if (c->ino == dir_ino)
            continue;
        if (efs_mode_is_dir(c->mode))
            recompute_dir_postorder(ex, c->ino);
        parent_add_child(d, c);
    }
}

void efs_export_recompute_rollups(struct efs_export *ex)
{
    if (!ex)
        return;
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (efs_mode_is_dir(ex->inodes[i].mode))
            inode_clear_rollups(&ex->inodes[i]);
        else
            inode_clear_rollups(&ex->inodes[i]);
    }
    child_idx_rebuild(ex);
    recompute_dir_postorder(ex, EFS_ROOT_INO);
}

int efs_export_format_stats(const struct efs_inode *dir, char *buf, size_t buflen)
{
    if (!dir || !buf || buflen == 0 || !efs_mode_is_dir(dir->mode))
        return -1;
    int n = snprintf(buf, buflen,
                     "imm_files=%llu\n"
                     "imm_dirs=%llu\n"
                     "imm_bytes=%llu\n"
                     "imm_tmin=%llu\n"
                     "imm_tmax=%llu\n"
                     "tree_files=%llu\n"
                     "tree_dirs=%llu\n"
                     "tree_bytes=%llu\n"
                     "tree_tmin=%llu\n"
                     "tree_tmax=%llu\n",
                     (unsigned long long)dir->imm_files,
                     (unsigned long long)dir->imm_dirs,
                     (unsigned long long)dir->imm_bytes,
                     (unsigned long long)dir->imm_tmin,
                     (unsigned long long)dir->imm_tmax,
                     (unsigned long long)dir->tree_files,
                     (unsigned long long)dir->tree_dirs,
                     (unsigned long long)dir->tree_bytes,
                     (unsigned long long)dir->tree_tmin,
                     (unsigned long long)dir->tree_tmax);
    if (n < 0 || (size_t)n >= buflen)
        return -1;
    return n;
}

/* Copy shared inode fields onto every hard-link row with the same ino.
 * Parent/name of each directory entry are preserved. */
static void sync_hardlink_attrs(struct efs_export *ex, efs_ino_t ino,
                                const struct efs_inode *src)
{
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (ex->inodes[i].ino != ino)
            continue;
        efs_ino_t parent = ex->inodes[i].parent;
        char name[EFS_MAX_NAME];
        memcpy(name, ex->inodes[i].name, EFS_MAX_NAME);
        /* Preserve per-dent identity; shared attrs include atime/size/times.
         * Rollups stay zero on file dents. */
        uint64_t imm_files = ex->inodes[i].imm_files;
        uint64_t imm_dirs = ex->inodes[i].imm_dirs;
        uint64_t tree_files = ex->inodes[i].tree_files;
        uint64_t tree_dirs = ex->inodes[i].tree_dirs;
        uint64_t imm_bytes = ex->inodes[i].imm_bytes;
        uint64_t tree_bytes = ex->inodes[i].tree_bytes;
        uint64_t imm_tmin = ex->inodes[i].imm_tmin;
        uint64_t imm_tmax = ex->inodes[i].imm_tmax;
        uint64_t tree_tmin = ex->inodes[i].tree_tmin;
        uint64_t tree_tmax = ex->inodes[i].tree_tmax;
        ex->inodes[i] = *src;
        ex->inodes[i].parent = parent;
        memcpy(ex->inodes[i].name, name, EFS_MAX_NAME);
        ex->inodes[i].imm_files = imm_files;
        ex->inodes[i].imm_dirs = imm_dirs;
        ex->inodes[i].tree_files = tree_files;
        ex->inodes[i].tree_dirs = tree_dirs;
        ex->inodes[i].imm_bytes = imm_bytes;
        ex->inodes[i].tree_bytes = tree_bytes;
        ex->inodes[i].imm_tmin = imm_tmin;
        ex->inodes[i].imm_tmax = imm_tmax;
        ex->inodes[i].tree_tmin = tree_tmin;
        ex->inodes[i].tree_tmax = tree_tmax;
    }
}

static void remove_chunks_for_ino(struct efs_export *ex, efs_ino_t ino)
{
    uint64_t j = 0;
    while (j < ex->chunk_count) {
        if (ex->chunks[j].ino == ino) {
            uint32_t cidx = ex->chunks[j].chunk_index;
            chunk_idx_del(ex, ino, cidx);
            uint64_t clast = ex->chunk_count - 1;
            if (j != clast) {
                chunk_idx_del(ex, ex->chunks[clast].ino, ex->chunks[clast].chunk_index);
                ex->chunks[j] = ex->chunks[clast];
                ex->chunk_count--;
                chunk_idx_put(ex, ex->chunks[j].ino, ex->chunks[j].chunk_index, j);
            } else {
                ex->chunk_count--;
            }
        } else {
            j++;
        }
    }
}

void efs_export_drop_chunks_from(struct efs_export *ex, efs_ino_t ino,
                                 uint32_t first_chunk)
{
    if (!ex)
        return;
    uint64_t j = 0;
    while (j < ex->chunk_count) {
        if (ex->chunks[j].ino == ino &&
            ex->chunks[j].chunk_index >= first_chunk) {
            uint32_t cidx = ex->chunks[j].chunk_index;
            chunk_idx_del(ex, ino, cidx);
            uint64_t clast = ex->chunk_count - 1;
            if (j != clast) {
                chunk_idx_del(ex, ex->chunks[clast].ino,
                              ex->chunks[clast].chunk_index);
                ex->chunks[j] = ex->chunks[clast];
                ex->chunk_count--;
                chunk_idx_put(ex, ex->chunks[j].ino, ex->chunks[j].chunk_index,
                              j);
            } else {
                ex->chunk_count--;
            }
        } else {
            j++;
        }
    }
}

/* Swap-remove inode array slot i; refresh indexes for the moved row.
 * Caller must already have applied rollup_sub and child_idx_del for slot i. */
static void remove_inode_slot(struct efs_export *ex, uint64_t i)
{
    efs_ino_t old_parent = ex->inodes[i].parent;
    char old_name[EFS_MAX_NAME];
    memcpy(old_name, ex->inodes[i].name, EFS_MAX_NAME);
    efs_ino_t old_ino = ex->inodes[i].ino;

    name_idx_del(ex, old_parent, old_name);

    uint64_t last = ex->inode_count - 1;
    if (i != last) {
        efs_ino_t moved_parent = ex->inodes[last].parent;
        idx_del(ex->ino_keys, ex->ino_vals, ex->ino_mask, ex->inodes[last].ino);
        name_idx_del(ex, ex->inodes[last].parent, ex->inodes[last].name);
        ex->inodes[i] = ex->inodes[last];
        ex->inode_count--;
        idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, ex->inodes[i].ino, i);
        name_idx_put(ex, ex->inodes[i].parent, ex->inodes[i].name, i);
        child_idx_replace_slot(ex, moved_parent, last, i);
    } else {
        ex->inode_count--;
    }

    /* If we removed the row that ino_idx pointed at (or any row), re-point
     * ino_idx to another remaining hard-link row when present. */
    uint64_t survivor = UINT64_MAX;
    for (uint64_t k = 0; k < ex->inode_count; k++) {
        if (ex->inodes[k].ino == old_ino) {
            survivor = k;
            break;
        }
    }
    if (survivor != UINT64_MAX)
        idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, old_ino, survivor);
    else
        idx_del(ex->ino_keys, ex->ino_vals, ex->ino_mask, old_ino);
}

void efs_export_init(struct efs_export *ex, efs_export_id_t id, const char *name)
{
    memset(ex, 0, sizeof(*ex));
    ex->id = id;
    strncpy(ex->name, name, EFS_MAX_NAME - 1);
    ex->next_ino = EFS_ROOT_INO + 1;

    ex->inode_capacity = 16;
    ex->inodes = calloc(ex->inode_capacity, sizeof(struct efs_inode));

    ex->chunk_capacity = 16;
    ex->chunks = calloc(ex->chunk_capacity, sizeof(struct efs_chunk_entry));

    struct efs_inode root = {0};
    root.ino = EFS_ROOT_INO;
    root.parent = EFS_ROOT_INO;
    root.mode = S_IFDIR | 0755;
    root.nlink = 2;
    strcpy(root.name, "/");
    time_now(&root.mtime);
    root.ctime = root.mtime;
    root.atime = root.mtime;
    ex->inodes[ex->inode_count++] = root;
    export_reindex(ex);
    child_idx_rebuild(ex);
}

void efs_export_free(struct efs_export *ex)
{
    if (!ex)
        return;
    free(ex->inodes);
    free(ex->chunks);
    idx_free(&ex->ino_keys, &ex->ino_vals, &ex->ino_mask);
    idx_free(&ex->name_keys, &ex->name_vals, &ex->name_mask);
    idx_free(&ex->chunk_keys, &ex->chunk_vals, &ex->chunk_mask);
    child_vecs_free(ex);
    efs_export_root_free(&ex->root);
    memset(ex, 0, sizeof(*ex));
}

int efs_export_lookup(struct efs_export *ex, efs_ino_t parent,
                      const char *name, struct efs_inode *out)
{
    if (!ex || !name)
        return EFS_ERR_INVAL;

    uint64_t pos = 0;
    if (name_idx_get(ex, parent, name, &pos) == 0) {
        if (out)
            *out = ex->inodes[pos];
        return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
}

int efs_export_get_inode(struct efs_export *ex, efs_ino_t ino,
                         struct efs_inode *out)
{
    if (!ex)
        return EFS_ERR_INVAL;
    struct efs_inode *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    if (out)
        *out = *p;
    return EFS_OK;
}

efs_ino_t efs_export_create_with_ino(struct efs_export *ex, efs_ino_t ino_num,
                                     efs_ino_t parent, uint32_t mode,
                                     uid_t uid, gid_t gid, const char *name)
{
    if (!ex || !name || !*name || ino_num == 0)
        return 0;
    if (strcmp(name, EFS_STATS_NAME) == 0)
        return 0;

    if (efs_export_lookup(ex, parent, name, NULL) == EFS_OK)
        return 0;

    if (efs_export_get_inode(ex, ino_num, NULL) == EFS_OK)
        return 0; /* ino already in use */

    if (ex->inode_count >= ex->inode_capacity) {
        uint64_t new_cap = ex->inode_capacity * 2;
        struct efs_inode *new = realloc(ex->inodes, new_cap * sizeof(struct efs_inode));
        if (!new)
            return 0;
        ex->inodes = new;
        ex->inode_capacity = new_cap;
    }
    if (export_ensure_inode_idx(ex) != 0)
        return 0;

    uint64_t pos = ex->inode_count++;
    struct efs_inode *ino = &ex->inodes[pos];
    memset(ino, 0, sizeof(*ino));
    ino->ino = ino_num;
    ino->parent = parent;
    ino->mode = mode;
    ino->uid = uid;
    ino->gid = gid;
    ino->nlink = 1;
    time_now(&ino->mtime);
    ino->ctime = ino->mtime;
    ino->atime = ino->mtime;
    strncpy(ino->name, name, EFS_MAX_NAME - 1);

    idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, ino_num, pos);
    name_idx_put(ex, parent, name, pos);
    child_idx_add(ex, parent, pos);

    if (efs_mode_is_dir(mode)) {
        ino->nlink = 2;
        struct efs_inode *p = inode_ptr(ex, parent);
        if (p)
            p->nlink++;
    }

    rollup_add_under(ex, parent, ino);
    return ino->ino;
}

efs_ino_t efs_export_create(struct efs_export *ex, efs_ino_t parent,
                              uint32_t mode, uid_t uid, gid_t gid, const char *name)
{
    if (!ex)
        return 0;
    efs_ino_t ino = efs_export_create_with_ino(ex, ex->next_ino, parent, mode, uid, gid, name);
    if (ino != 0)
        ex->next_ino++;
    return ino;
}

int efs_export_unlink_name(struct efs_export *ex, efs_ino_t parent, const char *name)
{
    if (!ex || !name)
        return EFS_ERR_INVAL;
    if (strcmp(name, EFS_STATS_NAME) == 0)
        return EFS_ERR_INVAL;

    uint64_t pos = 0;
    if (name_idx_get(ex, parent, name, &pos) != 0)
        return EFS_ERR_NOT_FOUND;

    struct efs_inode removed = ex->inodes[pos];
    efs_ino_t ino = removed.ino;
    uint32_t nlink = removed.nlink;
    if (nlink == 0)
        nlink = 1;
    nlink--;

    rollup_sub_under(ex, parent, &removed);
    child_idx_del(ex, parent, pos);
    remove_inode_slot(ex, pos);

    if (nlink == 0) {
        remove_chunks_for_ino(ex, ino);
        return EFS_OK;
    }

    /* Remaining hard links keep the decremented nlink. */
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (ex->inodes[i].ino == ino)
            ex->inodes[i].nlink = nlink;
    }
    return EFS_OK;
}

int efs_export_unlink(struct efs_export *ex, efs_ino_t ino)
{
    if (!ex)
        return EFS_ERR_INVAL;

    /* Remove every directory name for this inode, then chunks. */
    int found = 0;
    uint64_t i = 0;
    while (i < ex->inode_count) {
        if (ex->inodes[i].ino == ino) {
            found = 1;
            struct efs_inode removed = ex->inodes[i];
            rollup_sub_under(ex, removed.parent, &removed);
            child_idx_del(ex, removed.parent, i);
            remove_inode_slot(ex, i);
            /* slot i now holds a different row (or count shrank) */
        } else {
            i++;
        }
    }
    if (!found)
        return EFS_ERR_NOT_FOUND;
    remove_chunks_for_ino(ex, ino);
    return EFS_OK;
}

int efs_export_link(struct efs_export *ex, efs_ino_t src_ino,
                    efs_ino_t new_parent, const char *new_name)
{
    if (!ex || !new_name || !*new_name)
        return EFS_ERR_INVAL;
    if (strcmp(new_name, EFS_STATS_NAME) == 0)
        return EFS_ERR_INVAL;

    struct efs_inode *src = inode_ptr(ex, src_ino);
    if (!src)
        return EFS_ERR_NOT_FOUND;
    if (efs_mode_is_dir(src->mode))
        return EFS_ERR_INVAL;
    if (efs_export_lookup(ex, new_parent, new_name, NULL) == EFS_OK)
        return EFS_ERR_EXIST;
    struct efs_inode *pdir = inode_ptr(ex, new_parent);
    if (!pdir || !efs_mode_is_dir(pdir->mode))
        return EFS_ERR_INVAL;

    if (ex->inode_count >= ex->inode_capacity) {
        uint64_t new_cap = ex->inode_capacity * 2;
        struct efs_inode *new = realloc(ex->inodes, new_cap * sizeof(struct efs_inode));
        if (!new)
            return EFS_ERR_NOMEM;
        ex->inodes = new;
        ex->inode_capacity = new_cap;
    }
    if (export_ensure_inode_idx(ex) != 0)
        return EFS_ERR_NOMEM;

    /* Re-fetch after possible realloc. */
    src = inode_ptr(ex, src_ino);
    if (!src)
        return EFS_ERR_NOT_FOUND;

    uint32_t nlink = src->nlink + 1;
    sync_hardlink_attrs(ex, src_ino, src);
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (ex->inodes[i].ino == src_ino)
            ex->inodes[i].nlink = nlink;
    }
    src = inode_ptr(ex, src_ino);

    uint64_t pos = ex->inode_count++;
    struct efs_inode *dst = &ex->inodes[pos];
    *dst = *src;
    dst->parent = new_parent;
    dst->nlink = nlink;
    inode_clear_rollups(dst);
    strncpy(dst->name, new_name, EFS_MAX_NAME - 1);
    dst->name[EFS_MAX_NAME - 1] = '\0';
    time_now(&dst->ctime);

    /* Keep ino_idx pointing at an existing row; name index gets the new name. */
    name_idx_put(ex, new_parent, new_name, pos);
    child_idx_add(ex, new_parent, pos);
    rollup_add_under(ex, new_parent, dst);
    return EFS_OK;
}

int efs_export_set_size(struct efs_export *ex, efs_ino_t ino, uint64_t size)
{
    if (!ex)
        return EFS_ERR_INVAL;
    struct efs_inode *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    uint64_t old_size = p->size;
    p->size = size;
    /* Size changes update mtime with full nsec precision (not sec-only). */
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        p->mtime = (uint64_t)ts.tv_sec;
        p->mtime_nsec = (uint32_t)ts.tv_nsec;
    }
    sync_hardlink_attrs(ex, ino, p);
    if (!efs_mode_is_dir(p->mode) && size != old_size) {
        int64_t delta = (int64_t)size - (int64_t)old_size;
        for (uint64_t i = 0; i < ex->inode_count; i++) {
            if (ex->inodes[i].ino == ino)
                rollup_size_delta(ex, ex->inodes[i].parent, delta);
        }
    }
    rollup_touch_parents_of(ex, ino);
    return EFS_OK;
}

int efs_export_set_mode(struct efs_export *ex, efs_ino_t ino, uint32_t mode)
{
    if (!ex)
        return EFS_ERR_INVAL;
    struct efs_inode *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    p->mode = (p->mode & S_IFMT) | (mode & ~S_IFMT);
    /* POSIX: chmod updates ctime, not mtime (rsync -a relies on this). */
    time_now(&p->ctime);
    sync_hardlink_attrs(ex, ino, p);
    rollup_touch_parents_of(ex, ino);
    return EFS_OK;
}

int efs_export_set_owner(struct efs_export *ex, efs_ino_t ino, uid_t uid, gid_t gid)
{
    if (!ex)
        return EFS_ERR_INVAL;
    struct efs_inode *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    if (uid != (uid_t)-1)
        p->uid = uid;
    if (gid != (gid_t)-1)
        p->gid = gid;
    /* POSIX: chown updates ctime, not mtime. */
    time_now(&p->ctime);
    sync_hardlink_attrs(ex, ino, p);
    rollup_touch_parents_of(ex, ino);
    return EFS_OK;
}

int efs_export_set_mtime_ns(struct efs_export *ex, efs_ino_t ino,
                            uint64_t mtime, uint32_t mtime_nsec)
{
    if (!ex)
        return EFS_ERR_INVAL;
    struct efs_inode *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    if (mtime_nsec >= 1000000000u)
        mtime_nsec = 0;
    p->mtime = mtime;
    p->mtime_nsec = mtime_nsec;
    sync_hardlink_attrs(ex, ino, p);
    rollup_touch_parents_of(ex, ino);
    return EFS_OK;
}

int efs_export_set_mtime(struct efs_export *ex, efs_ino_t ino, uint64_t mtime)
{
    return efs_export_set_mtime_ns(ex, ino, mtime, 0);
}

int efs_export_set_atime(struct efs_export *ex, efs_ino_t ino, uint64_t atime)
{
    if (!ex)
        return EFS_ERR_INVAL;
    struct efs_inode *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    p->atime = atime;
    sync_hardlink_attrs(ex, ino, p);
    rollup_touch_parents_of(ex, ino);
    return EFS_OK;
}

int efs_export_rename(struct efs_export *ex, efs_ino_t ino,
                      efs_ino_t new_parent, const char *new_name)
{
    if (!ex || !new_name)
        return EFS_ERR_INVAL;
    if (strcmp(new_name, EFS_STATS_NAME) == 0)
        return EFS_ERR_INVAL;

    struct efs_inode *src = inode_ptr(ex, ino);
    if (!src)
        return EFS_ERR_NOT_FOUND;

    struct efs_inode *dst_parent = inode_ptr(ex, new_parent);
    if (!dst_parent || !efs_mode_is_dir(dst_parent->mode))
        return EFS_ERR_INVAL;

    /* Prevent moving a directory into itself or its descendants. */
    if (efs_mode_is_dir(src->mode)) {
        efs_ino_t p = new_parent;
        while (p != EFS_ROOT_INO && p != src->ino) {
            struct efs_inode *pi = inode_ptr(ex, p);
            if (!pi || pi->parent == p)
                break;
            p = pi->parent;
        }
        if (p == src->ino)
            return EFS_ERR_INVAL;
    }

    struct efs_inode dst_copy;
    int have_dst = 0;
    {
        uint64_t dpos = 0;
        if (name_idx_get(ex, new_parent, new_name, &dpos) == 0 &&
            ex->inodes[dpos].ino != ino) {
            dst_copy = ex->inodes[dpos];
            have_dst = 1;
        }
    }

    if (have_dst) {
        int src_dir = efs_mode_is_dir(src->mode);
        int dst_dir = efs_mode_is_dir(dst_copy.mode);

        if (src_dir != dst_dir)
            return EFS_ERR_INVAL;

        if (src_dir) {
            struct efs_child_vec *cv = child_vec_get(ex, dst_copy.ino, 0);
            if (cv && cv->count > 0)
                return EFS_ERR_NOT_EMPTY;
        }
        /* efs_export_unlink swap-removes the destination entry, which can
         * move or overwrite the entry `src` points to. Re-find it after. */
        efs_export_unlink(ex, dst_copy.ino);
        src = inode_ptr(ex, ino);
        if (!src)
            return EFS_ERR_NOT_FOUND;
    }

    uint64_t slot = (uint64_t)(src - ex->inodes);
    efs_ino_t old_parent = src->parent;
    if (old_parent != new_parent || strcmp(src->name, new_name) != 0) {
        struct efs_inode snap = *src;
        if (old_parent != new_parent)
            rollup_sub_under(ex, old_parent, &snap);
        child_idx_del(ex, old_parent, slot);
        name_idx_del(ex, src->parent, src->name);
        strncpy(src->name, new_name, EFS_MAX_NAME - 1);
        src->name[EFS_MAX_NAME - 1] = '\0';
        src->parent = new_parent;
        /* Preserve mtime across rename (rsync partial → final); bump ctime only. */
        time_now(&src->ctime);
        name_idx_put(ex, src->parent, src->name, slot);
        child_idx_add(ex, new_parent, slot);
        if (old_parent != new_parent)
            rollup_add_under(ex, new_parent, src);
        else
            recompute_times_up(ex, new_parent);
    }
    return EFS_OK;
}

int efs_export_set_chunk(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         const efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS],
                         const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    if (!ex)
        return EFS_ERR_INVAL;

    uint64_t pos = 0;
    if (chunk_idx_get(ex, ino, chunk_index, &pos) == 0) {
        memcpy(ex->chunks[pos].fragment_nodes, fragment_nodes,
               sizeof(efs_node_id_t) * EFS_NUM_FRAGMENTS);
        memcpy(ex->chunks[pos].checksums, checksums,
               EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
        return EFS_OK;
    }

    if (ex->chunk_count >= ex->chunk_capacity) {
        uint64_t new_cap = ex->chunk_capacity * 2;
        struct efs_chunk_entry *new = realloc(ex->chunks, new_cap * sizeof(struct efs_chunk_entry));
        if (!new)
            return EFS_ERR_NOMEM;
        ex->chunks = new;
        ex->chunk_capacity = new_cap;
    }
    if (export_ensure_chunk_idx(ex) != 0)
        return EFS_ERR_NOMEM;

    pos = ex->chunk_count++;
    struct efs_chunk_entry *ce = &ex->chunks[pos];
    memset(ce, 0, sizeof(*ce));
    ce->ino = ino;
    ce->chunk_index = chunk_index;
    memcpy(ce->fragment_nodes, fragment_nodes, sizeof(efs_node_id_t) * EFS_NUM_FRAGMENTS);
    memcpy(ce->checksums, checksums, EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
    chunk_idx_put(ex, ino, chunk_index, pos);
    return EFS_OK;
}

int efs_export_get_chunk(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         struct efs_chunk_entry *out)
{
    if (!ex)
        return EFS_ERR_INVAL;

    uint64_t pos = 0;
    if (chunk_idx_get(ex, ino, chunk_index, &pos) == 0) {
        if (out)
            *out = ex->chunks[pos];
        return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
}

static int grow_inodes(struct efs_export *ex)
{
    if (ex->inode_count < ex->inode_capacity)
        return EFS_OK;
    uint64_t new_cap = ex->inode_capacity ? ex->inode_capacity * 2 : 16;
    struct efs_inode *n = realloc(ex->inodes, new_cap * sizeof(struct efs_inode));
    if (!n)
        return EFS_ERR_NOMEM;
    ex->inodes = n;
    ex->inode_capacity = new_cap;
    return EFS_OK;
}

static int grow_chunks(struct efs_export *ex)
{
    if (ex->chunk_count < ex->chunk_capacity)
        return EFS_OK;
    uint64_t new_cap = ex->chunk_capacity ? ex->chunk_capacity * 2 : 16;
    struct efs_chunk_entry *n = realloc(ex->chunks, new_cap * sizeof(struct efs_chunk_entry));
    if (!n)
        return EFS_ERR_NOMEM;
    ex->chunks = n;
    ex->chunk_capacity = new_cap;
    return EFS_OK;
}

int efs_export_merge(struct efs_export *ex, const struct efs_export *inc)
{
    if (!ex || !inc)
        return EFS_ERR_INVAL;

    if (!ex->ino_keys && export_reindex(ex) != 0)
        return EFS_ERR_NOMEM;

    /* Bootstrap PUT_META used to leave ex->name empty ("") while the blob
     * carried the real export name — adopt it so list-exports is not blank. */
    if (inc->name[0] &&
        (ex->name[0] == '\0' || strcmp(ex->name, "pending") == 0)) {
        strncpy(ex->name, inc->name, EFS_MAX_NAME - 1);
        ex->name[EFS_MAX_NAME - 1] = '\0';
    }
    if (inc->id != 0 &&
        (ex->id == 0 || ex->name[0] == '\0' || strcmp(ex->name, "pending") == 0 ||
         ex->id == inc->id))
        ex->id = inc->id;

    /* Merge inodes: add new ones, update existing ones when the incoming
     * entry is newer (higher mtime, or equal mtime but larger size so a
     * growing write is not lost). Hard-link rows share an ino but have
     * distinct (parent,name); they are merged by directory name. Inodes
     * present on the server but absent from the client are left untouched. */
    for (uint64_t i = 0; i < inc->inode_count; i++) {
        const struct efs_inode *ci = &inc->inodes[i];
        uint64_t name_pos = 0;
        int have_name = (name_idx_get(ex, ci->parent, ci->name, &name_pos) == 0);

        if (have_name) {
            struct efs_inode *cur = &ex->inodes[name_pos];
            if (ci->mtime > cur->mtime ||
                (ci->mtime == cur->mtime && ci->size > cur->size) ||
                ci->nlink > cur->nlink) {
                *cur = *ci;
                sync_hardlink_attrs(ex, ci->ino, cur);
            }
            continue;
        }

        struct efs_inode *cur = inode_ptr(ex, ci->ino);
        if (!cur) {
            if (grow_inodes(ex) != 0)
                return EFS_ERR_NOMEM;
            if (export_ensure_inode_idx(ex) != 0)
                return EFS_ERR_NOMEM;
            uint64_t pos = ex->inode_count++;
            ex->inodes[pos] = *ci;
            idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, ci->ino, pos);
            name_idx_put(ex, ci->parent, ci->name, pos);
        } else if (cur->parent != ci->parent || strcmp(cur->name, ci->name) != 0) {
            /* Additional hard-link name for an inode already on the server. */
            if (grow_inodes(ex) != 0)
                return EFS_ERR_NOMEM;
            if (export_ensure_inode_idx(ex) != 0)
                return EFS_ERR_NOMEM;
            uint64_t pos = ex->inode_count++;
            ex->inodes[pos] = *ci;
            name_idx_put(ex, ci->parent, ci->name, pos);
            if (ci->mtime > cur->mtime ||
                (ci->mtime == cur->mtime && ci->size > cur->size) ||
                ci->nlink > cur->nlink) {
                sync_hardlink_attrs(ex, ci->ino, ci);
            } else {
                /* Keep server attrs; just adopt the higher nlink if needed. */
                struct efs_inode *canon = inode_ptr(ex, ci->ino);
                if (canon && ci->nlink > canon->nlink) {
                    for (uint64_t k = 0; k < ex->inode_count; k++) {
                        if (ex->inodes[k].ino == ci->ino)
                            ex->inodes[k].nlink = ci->nlink;
                    }
                }
            }
        } else if (ci->mtime > cur->mtime ||
                   (ci->mtime == cur->mtime && ci->size > cur->size)) {
            *cur = *ci;
            sync_hardlink_attrs(ex, ci->ino, cur);
        }
    }

    /* Merge chunk entries: add new ones, replace existing ones (a rewrite of
     * a chunk carries fresh checksums). Chunks the client does not know about
     * are preserved. */
    for (uint64_t i = 0; i < inc->chunk_count; i++) {
        const struct efs_chunk_entry *cc = &inc->chunks[i];
        uint64_t pos = 0;
        if (chunk_idx_get(ex, cc->ino, cc->chunk_index, &pos) == 0) {
            ex->chunks[pos] = *cc;
        } else {
            if (grow_chunks(ex) != 0)
                return EFS_ERR_NOMEM;
            if (export_ensure_chunk_idx(ex) != 0)
                return EFS_ERR_NOMEM;
            pos = ex->chunk_count++;
            ex->chunks[pos] = *cc;
            chunk_idx_put(ex, cc->ino, cc->chunk_index, pos);
        }
    }

    if (inc->next_ino > ex->next_ino)
        ex->next_ino = inc->next_ino;

    /* Rollups are derived; rebuild after merge so newer-wins cannot leave
     * stale aggregates relative to the combined child set. */
    efs_export_recompute_rollups(ex);
    return EFS_OK;
}

static int write_u32(FILE *f, uint32_t v)
{
    return fwrite(&v, sizeof(v), 1, f) == 1 ? 0 : -1;
}

static int write_u64(FILE *f, uint64_t v)
{
    return fwrite(&v, sizeof(v), 1, f) == 1 ? 0 : -1;
}

static int write_str(FILE *f, const char *s)
{
    uint32_t len = (uint32_t)strlen(s);
    if (write_u32(f, len) != 0)
        return -1;
    if (len > 0 && fwrite(s, len, 1, f) != 1)
        return -1;
    return 0;
}

static int read_u32(FILE *f, uint32_t *v)
{
    return fread(v, sizeof(*v), 1, f) == 1 ? 0 : -1;
}

static int read_u64(FILE *f, uint64_t *v)
{
    return fread(v, sizeof(*v), 1, f) == 1 ? 0 : -1;
}

static int read_str(FILE *f, char *s, size_t max)
{
    uint32_t len;
    if (read_u32(f, &len) != 0)
        return -1;
    if (len >= max)
        return -1;
    if (len > 0 && fread(s, len, 1, f) != 1)
        return -1;
    s[len] = '\0';
    return 0;
}

int efs_export_serialize(struct efs_export *ex, char **buf, size_t *len)
{
    if (!ex || !buf || !len)
        return EFS_ERR_INVAL;

    FILE *f = open_memstream(buf, len);
    if (!f)
        return EFS_ERR_NOMEM;

    fwrite(EFS_META_MAGIC, 4, 1, f);
    write_u32(f, EFS_META_VERSION);
    write_u32(f, ex->id);
    write_str(f, ex->name);
    write_u64(f, ex->next_ino);

    write_u32(f, (uint32_t)ex->inode_count);
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        struct efs_inode *ino = &ex->inodes[i];
        write_u64(f, ino->ino);
        write_u64(f, ino->parent);
        write_u32(f, ino->mode);
        write_u32(f, (uint32_t)ino->uid);
        write_u32(f, (uint32_t)ino->gid);
        write_u64(f, ino->size);
        write_u64(f, ino->mtime);
        write_u32(f, ino->mtime_nsec);
        write_u64(f, ino->ctime);
        write_u64(f, ino->atime);
        write_u32(f, ino->nlink);
        write_str(f, ino->name);
        if (efs_mode_is_dir(ino->mode)) {
            write_u64(f, ino->imm_files);
            write_u64(f, ino->imm_dirs);
            write_u64(f, ino->tree_files);
            write_u64(f, ino->tree_dirs);
            write_u64(f, ino->imm_bytes);
            write_u64(f, ino->tree_bytes);
            write_u64(f, ino->imm_tmin);
            write_u64(f, ino->imm_tmax);
            write_u64(f, ino->tree_tmin);
            write_u64(f, ino->tree_tmax);
        }
    }

    write_u32(f, (uint32_t)ex->chunk_count);
    for (uint64_t i = 0; i < ex->chunk_count; i++) {
        struct efs_chunk_entry *ce = &ex->chunks[i];
        write_u64(f, ce->ino);
        write_u32(f, ce->chunk_index);
        fwrite(ce->fragment_nodes, sizeof(efs_node_id_t), EFS_NUM_FRAGMENTS, f);
        fwrite(ce->checksums, EFS_HASH_SIZE, EFS_NUM_FRAGMENTS, f);
    }

    fclose(f);
    return EFS_OK;
}

int efs_export_deserialize(struct efs_export *ex, const char *buf, size_t len)
{
    if (!ex || !buf)
        return EFS_ERR_INVAL;

    FILE *f = fmemopen((void *)buf, len, "r");
    if (!f)
        return EFS_ERR_IO;

    char magic[4];
    if (fread(magic, 4, 1, f) != 1 || memcmp(magic, EFS_META_MAGIC, 4) != 0) {
        fclose(f);
        return EFS_ERR_PROTO;
    }

    uint32_t version;
    if (read_u32(f, &version) != 0 ||
        (version != 2 && version != 3 && version != 4)) {
        fclose(f);
        return EFS_ERR_PROTO;
    }

    efs_export_free(ex);
    efs_export_init(ex, 0, "");
    ex->meta_fragmented = 0;

    read_u32(f, &ex->id);
    read_str(f, ex->name, EFS_MAX_NAME);
    read_u64(f, &ex->next_ino);

    uint32_t inode_count;
    if (read_u32(f, &inode_count) != 0) {
        fclose(f);
        return EFS_ERR_PROTO;
    }

    /* Replace the empty root from init with the deserialized table. */
    ex->inode_count = 0;
    child_vecs_free(ex);
    for (uint32_t i = 0; i < inode_count; i++) {
        if (ex->inode_count >= ex->inode_capacity) {
            uint64_t new_cap = ex->inode_capacity * 2;
            struct efs_inode *new = realloc(ex->inodes, new_cap * sizeof(struct efs_inode));
            if (!new) {
                fclose(f);
                return EFS_ERR_NOMEM;
            }
            ex->inodes = new;
            ex->inode_capacity = new_cap;
        }
        struct efs_inode *ino = &ex->inodes[ex->inode_count++];
        memset(ino, 0, sizeof(*ino));
        read_u64(f, &ino->ino);
        read_u64(f, &ino->parent);
        read_u32(f, &ino->mode);
        if (version >= 2) {
            uint32_t uid, gid;
            read_u32(f, &uid);
            read_u32(f, &gid);
            ino->uid = (uid_t)uid;
            ino->gid = (gid_t)gid;
        } else {
            ino->uid = 0;
            ino->gid = 0;
        }
        read_u64(f, &ino->size);
        read_u64(f, &ino->mtime);
        if (version >= 3) {
            read_u32(f, &ino->mtime_nsec);
            if (ino->mtime_nsec >= 1000000000u)
                ino->mtime_nsec = 0;
        } else {
            ino->mtime_nsec = 0;
        }
        read_u64(f, &ino->ctime);
        if (version >= 4) {
            read_u64(f, &ino->atime);
        } else {
            ino->atime = ino->mtime;
        }
        read_u32(f, &ino->nlink);
        read_str(f, ino->name, EFS_MAX_NAME);
        if (version >= 4 && efs_mode_is_dir(ino->mode)) {
            read_u64(f, &ino->imm_files);
            read_u64(f, &ino->imm_dirs);
            read_u64(f, &ino->tree_files);
            read_u64(f, &ino->tree_dirs);
            read_u64(f, &ino->imm_bytes);
            read_u64(f, &ino->tree_bytes);
            read_u64(f, &ino->imm_tmin);
            read_u64(f, &ino->imm_tmax);
            read_u64(f, &ino->tree_tmin);
            read_u64(f, &ino->tree_tmax);
        }
    }

    uint32_t chunk_count;
    if (read_u32(f, &chunk_count) != 0) {
        fclose(f);
        return EFS_ERR_PROTO;
    }

    ex->chunk_count = 0;
    for (uint32_t i = 0; i < chunk_count; i++) {
        if (ex->chunk_count >= ex->chunk_capacity) {
            uint64_t new_cap = ex->chunk_capacity * 2;
            struct efs_chunk_entry *new = realloc(ex->chunks, new_cap * sizeof(struct efs_chunk_entry));
            if (!new) {
                fclose(f);
                return EFS_ERR_NOMEM;
            }
            ex->chunks = new;
            ex->chunk_capacity = new_cap;
        }
        struct efs_chunk_entry *ce = &ex->chunks[ex->chunk_count++];
        read_u64(f, &ce->ino);
        read_u32(f, &ce->chunk_index);
        fread(ce->fragment_nodes, sizeof(efs_node_id_t), EFS_NUM_FRAGMENTS, f);
        fread(ce->checksums, EFS_HASH_SIZE, EFS_NUM_FRAGMENTS, f);
    }

    fclose(f);
    if (export_reindex(ex) != 0)
        return EFS_ERR_NOMEM;
    efs_export_recompute_rollups(ex);
    return EFS_OK;
}

int efs_meta_blob_is_root(const char *buf, size_t len)
{
    return buf && len >= 4 && memcmp(buf, EFS_META_ROOT_MAGIC, 4) == 0;
}

int efs_meta_blob_is_export(const char *buf, size_t len)
{
    return buf && len >= 4 && memcmp(buf, EFS_META_MAGIC, 4) == 0;
}

uint32_t efs_meta_page_count_for_blob(uint32_t blob_len)
{
    if (blob_len == 0)
        return 0;
    return (blob_len + EFS_CHUNK_SIZE - 1) / EFS_CHUNK_SIZE;
}

int efs_meta_extract_page(const char *blob, uint32_t blob_len, uint32_t page_index,
                          uint8_t page_out[EFS_CHUNK_SIZE])
{
    if (!blob || !page_out)
        return EFS_ERR_INVAL;
    uint32_t pages = efs_meta_page_count_for_blob(blob_len);
    if (page_index >= pages)
        return EFS_ERR_INVAL;
    memset(page_out, 0, EFS_CHUNK_SIZE);
    uint32_t off = page_index * EFS_CHUNK_SIZE;
    uint32_t n = blob_len - off;
    if (n > EFS_CHUNK_SIZE)
        n = EFS_CHUNK_SIZE;
    memcpy(page_out, blob + off, n);
    return EFS_OK;
}

int efs_meta_assemble_blob(const struct efs_export_root *root,
                           const uint8_t pages[][EFS_CHUNK_SIZE],
                           char **blob_out, size_t *blob_len_out)
{
    if (!root || !pages || !blob_out || !blob_len_out)
        return EFS_ERR_INVAL;
    if (root->page_count == 0 || root->blob_len == 0) {
        *blob_out = NULL;
        *blob_len_out = 0;
        return EFS_ERR_INVAL;
    }
    char *blob = malloc(root->blob_len);
    if (!blob)
        return EFS_ERR_NOMEM;
    for (uint32_t i = 0; i < root->page_count; i++) {
        uint32_t off = i * EFS_CHUNK_SIZE;
        uint32_t n = root->blob_len - off;
        if (n > EFS_CHUNK_SIZE)
            n = EFS_CHUNK_SIZE;
        memcpy(blob + off, pages[i], n);
    }
    *blob_out = blob;
    *blob_len_out = root->blob_len;
    return EFS_OK;
}

void efs_export_root_free(struct efs_export_root *root)
{
    if (!root)
        return;
    free(root->page_checksums);
    root->page_checksums = NULL;
    root->page_count = 0;
}

void efs_export_root_move(struct efs_export_root *dst, struct efs_export_root *src)
{
    if (!dst || !src)
        return;
    efs_export_root_free(dst);
    *dst = *src;
    memset(src, 0, sizeof(*src));
}

int efs_export_root_copy(struct efs_export_root *dst, const struct efs_export_root *src)
{
    if (!dst || !src)
        return EFS_ERR_INVAL;
    efs_export_root_free(dst);
    *dst = *src;
    dst->page_checksums = NULL;
    if (src->page_count == 0 || !src->page_checksums)
        return EFS_OK;
    size_t n = (size_t)src->page_count * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE;
    dst->page_checksums = malloc(n);
    if (!dst->page_checksums)
        return EFS_ERR_NOMEM;
    memcpy(dst->page_checksums, src->page_checksums, n);
    return EFS_OK;
}

int efs_export_root_prepare(struct efs_export_root *root,
                            const struct efs_export *ex,
                            uint64_t generation,
                            uint32_t blob_len)
{
    if (!root || !ex)
        return EFS_ERR_INVAL;
    efs_export_root_free(root);
    memset(root, 0, sizeof(*root));
    root->version = EFS_META_ROOT_VERSION;
    root->id = ex->id;
    strncpy(root->name, ex->name, EFS_MAX_NAME - 1);
    root->next_ino = ex->next_ino;
    root->generation = generation;
    root->blob_len = blob_len;
    root->page_count = efs_meta_page_count_for_blob(blob_len);
    if (root->page_count > EFS_META_MAX_PAGES)
        return EFS_ERR_INVAL;
    if (root->page_count > 0) {
        size_t n = (size_t)root->page_count * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE;
        root->page_checksums = calloc(1, n);
        if (!root->page_checksums)
            return EFS_ERR_NOMEM;
    }
    return EFS_OK;
}

int efs_export_root_serialize(const struct efs_export_root *root,
                              char **buf, size_t *len)
{
    if (!root || !buf || !len)
        return EFS_ERR_INVAL;
    if (root->page_count > EFS_META_MAX_PAGES)
        return EFS_ERR_INVAL;
    if (root->page_count > 0 && !root->page_checksums)
        return EFS_ERR_INVAL;

    FILE *f = open_memstream(buf, len);
    if (!f)
        return EFS_ERR_NOMEM;

    fwrite(EFS_META_ROOT_MAGIC, 4, 1, f);
    write_u32(f, root->version);
    write_u32(f, root->id);
    write_str(f, root->name);
    write_u64(f, root->next_ino);
    write_u64(f, root->generation);
    write_u32(f, root->blob_len);
    write_u32(f, root->page_count);
    if (root->page_count > 0) {
        size_t n = (size_t)root->page_count * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE;
        fwrite(root->page_checksums, 1, n, f);
    }
    fclose(f);
    return EFS_OK;
}

int efs_export_root_deserialize(struct efs_export_root *root,
                                const char *buf, size_t len)
{
    if (!root || !buf)
        return EFS_ERR_INVAL;

    FILE *f = fmemopen((void *)buf, len, "r");
    if (!f)
        return EFS_ERR_IO;

    char magic[4];
    if (fread(magic, 4, 1, f) != 1 || memcmp(magic, EFS_META_ROOT_MAGIC, 4) != 0) {
        fclose(f);
        return EFS_ERR_PROTO;
    }

    efs_export_root_free(root);
    memset(root, 0, sizeof(*root));
    uint32_t version = 0;
    if (read_u32(f, &version) != 0 || version != EFS_META_ROOT_VERSION) {
        fclose(f);
        return EFS_ERR_PROTO;
    }
    root->version = version;
    read_u32(f, &root->id);
    read_str(f, root->name, EFS_MAX_NAME);
    read_u64(f, &root->next_ino);
    read_u64(f, &root->generation);
    read_u32(f, &root->blob_len);
    read_u32(f, &root->page_count);
    if (root->page_count > EFS_META_MAX_PAGES ||
        root->page_count != efs_meta_page_count_for_blob(root->blob_len)) {
        fclose(f);
        return EFS_ERR_PROTO;
    }
    if (root->page_count > 0) {
        size_t n = (size_t)root->page_count * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE;
        root->page_checksums = malloc(n);
        if (!root->page_checksums) {
            fclose(f);
            return EFS_ERR_NOMEM;
        }
        if (fread(root->page_checksums, 1, n, f) != n) {
            free(root->page_checksums);
            root->page_checksums = NULL;
            fclose(f);
            return EFS_ERR_PROTO;
        }
    }
    fclose(f);
    return EFS_OK;
}

int efs_export_load(struct efs_export *ex, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return EFS_ERR_IO;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return EFS_ERR_IO;
    }

    char *buf = malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return EFS_ERR_NOMEM;
    }

    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return EFS_ERR_IO;
    }
    fclose(f);

    int rc;
    if (efs_meta_blob_is_root(buf, (size_t)len)) {
        struct efs_export_root root;
        memset(&root, 0, sizeof(root));
        rc = efs_export_root_deserialize(&root, buf, (size_t)len);
        if (rc == EFS_OK) {
            /* Keep a minimal export; caller rebuilds bulk from pages. */
            efs_export_free(ex);
            efs_export_init(ex, root.id, root.name);
            ex->next_ino = root.next_ino;
            ex->meta_fragmented = 1;
            efs_export_root_move(&ex->root, &root);
            ex->meta_needs_rebuild = (ex->root.page_count > 0);
        }
    } else {
        rc = efs_export_deserialize(ex, buf, (size_t)len);
        if (rc == EFS_OK)
            ex->meta_fragmented = 0;
    }
    free(buf);
    return rc;
}

int efs_export_save(struct efs_export *ex, const char *path)
{
    char *buf = NULL;
    size_t len = 0;
    int rc;
    if (ex->meta_fragmented)
        rc = efs_export_root_serialize(&ex->root, &buf, &len);
    else
        rc = efs_export_serialize(ex, &buf, &len);
    if (rc != EFS_OK)
        return rc;

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return EFS_ERR_IO;
    }

    if (fwrite(buf, 1, len, f) != len) {
        free(buf);
        fclose(f);
        return EFS_ERR_IO;
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(buf);
    return EFS_OK;
}
