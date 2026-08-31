#include "efs/metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EFS_META_MAGIC "EFSM"
#define EFS_META_ROOT_MAGIC "EFSR"
/* EFSR version constants (EFS_META_ROOT_VERSION_V1..V7, EFS_META_ROOT_VERSION)
 * live in metadata.h (shared with the client flush). */
/* EFSM: v2: uid/gid. v3: mtime_nsec after mtime. v4: atime + dir rollups.
 * v5: fixed-size inode records + pack fields; chunk table is a separate
 * page region so creates do not shift chunk pages.
 * v6: compact inodes (no name / tree rollups) + packed dentries.
 * v7: dentry region page-aligned within the inode region (offset derived
 * from inode_count) so a create no longer shifts/re-dirties the dentry
 * tail — O(1) metadata flush per create instead of O(table).
 * EFS_META_VERSION is in metadata.h (shared with the client incremental
 * serialize). */
/* EFS_INODE_WIRE_SIZE is in metadata.h (EFSM v5). */

static size_t dentry_bytes_for(const struct efs_export *ex, uint64_t extra_inodes);
static size_t dentry_rec_len(const char *name);
static void dentry_bytes_add(struct efs_export *ex, const char *name);
static void dentry_bytes_sub(struct efs_export *ex, const char *name);
static void dentry_bytes_recompute(struct efs_export *ex);

static void time_now(uint64_t *t)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *t = (uint64_t)ts.tv_sec;
}

/* POSIX link/rename bump ctime. Second resolution would hide a same-second
 * link, so advance by 1 when the clock has not moved. */
static void inode_bump_ctime(struct efs_inode *p)
{
    if (!p)
        return;
    uint64_t old = p->ctime;
    time_now(&p->ctime);
    if (p->ctime <= old)
        p->ctime = old + 1;
}

static struct efs_inode *inode_ptr(struct efs_export *ex, efs_ino_t ino);
static void flush_mark_ptr(struct efs_export *ex, const struct efs_inode *p);

static void stamp_ctime_loaded(struct efs_export *ex, efs_ino_t ino, uint64_t ct)
{
    if (!ex || !ino)
        return;
    /* Common case (rsync temp→final, nlink=1): the row we already hold.
     * The old full-table scan was O(inodes) per rename and made a grown
     * export's rsync O(n²). */
    struct efs_inode *p = inode_ptr(ex, ino);
    if (p) {
        p->ctime = ct;
        flush_mark_ptr(ex, p);
        if (p->nlink <= 1)
            return;
    }
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (ex->inodes[i].ino == ino) {
            ex->inodes[i].ctime = ct;
            efs_export_flush_mark_ino_slot(ex, i);
        }
    }
    if (!ex->shard_tabs)
        return;
    for (uint32_t s = 1; s < ex->shard_tab_cap; s++) {
        struct efs_export *t = ex->shard_tabs[s];
        if (!t)
            continue;
        for (uint64_t i = 0; i < t->inode_count; i++) {
            if (t->inodes[i].ino == ino) {
                t->inodes[i].ctime = ct;
                efs_export_flush_mark_ino_slot(t, i);
            }
        }
    }
}

static void parent_touch(struct efs_export *ex, efs_ino_t parent)
{
    struct efs_inode *p = inode_ptr(ex, parent);
    if (!p)
        return;
    time_now(&p->mtime);
    p->ctime = p->mtime;
    p->mtime_nsec = 0;
    flush_mark_ptr(ex, p);
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

static int export_is_sharded_root(struct efs_export *ex);

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

/* Mix before taking the slot. Client inos are (tag<<40)|counter; without a
 * mix, tag sits above a 22-bit mask so every namespace lands on `counter` and
 * a second-tree lookup walks the whole occupied run (1.3M probes, ~60% CPU). */
static uint64_t idx_slot(uint64_t key, uint64_t mask)
{
    return hash_mix(key) & mask;
}

static int idx_put(uint64_t *keys, uint64_t *vals, uint64_t mask, uint64_t key, uint64_t val)
{
    if (!keys || mask == 0 || key == 0)
        return -1;
    uint64_t i = idx_slot(key, mask);
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
    if (!keys || mask == 0 || key == 0)
        return -1;
    uint64_t i = idx_slot(key, mask);
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
    if (!keys || mask == 0 || key == 0)
        return;
    uint64_t i = idx_slot(key, mask);
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
    /* nlink=0 open-fd ghosts keep the inode row but have no directory name. */
    if (!name || !name[0] || parent == 0)
        return -1;
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
    if (!ex->chunk_keys || !ex->chunk_vals || !ex->chunks || ex->chunk_mask == 0)
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

/* --- per-ino live chunk count (ino -> count in this table) --- */
static uint32_t icnt_get(const struct efs_export *ex, efs_ino_t ino)
{
    if (!ex->icnt_keys || ex->icnt_mask == 0 || ino == 0)
        return 0;
    uint64_t i = idx_slot(ino, ex->icnt_mask);
    for (uint64_t n = 0; n <= ex->icnt_mask; n++) {
        if (ex->icnt_keys[i] == 0)
            return 0;
        if (ex->icnt_keys[i] == ino)
            return ex->icnt_vals[i];
        i = (i + 1) & ex->icnt_mask;
    }
    return 0;
}

static void icnt_put(struct efs_export *ex, efs_ino_t ino, uint32_t count)
{
    uint64_t i = idx_slot(ino, ex->icnt_mask);
    for (uint64_t n = 0; n <= ex->icnt_mask; n++) {
        if (ex->icnt_keys[i] == 0 || ex->icnt_keys[i] == ino) {
            ex->icnt_keys[i] = ino;
            ex->icnt_vals[i] = count;
            return;
        }
        i = (i + 1) & ex->icnt_mask;
    }
}

static void icnt_inc(struct efs_export *ex, efs_ino_t ino)
{
    if (!ex->icnt_keys || ex->icnt_mask == 0 || ino == 0)
        return;
    uint64_t i = idx_slot(ino, ex->icnt_mask);
    for (uint64_t n = 0; n <= ex->icnt_mask; n++) {
        if (ex->icnt_keys[i] == 0) {
            ex->icnt_keys[i] = ino;
            ex->icnt_vals[i] = 1;
            return;
        }
        if (ex->icnt_keys[i] == ino) {
            ex->icnt_vals[i]++;
            return;
        }
        i = (i + 1) & ex->icnt_mask;
    }
}

static void icnt_dec(struct efs_export *ex, efs_ino_t ino)
{
    if (!ex->icnt_keys || ex->icnt_mask == 0 || ino == 0)
        return;
    uint64_t i = idx_slot(ino, ex->icnt_mask);
    for (uint64_t n = 0; n <= ex->icnt_mask; n++) {
        if (ex->icnt_keys[i] == 0)
            return;
        if (ex->icnt_keys[i] == ino)
            break;
        i = (i + 1) & ex->icnt_mask;
    }
    if (ex->icnt_keys[i] != ino)
        return;
    if (ex->icnt_vals[i] > 1) {
        ex->icnt_vals[i]--;
        return;
    }
    /* count hits 0: delete with backfill (mirror chunk_idx_del). */
    ex->icnt_keys[i] = 0;
    ex->icnt_vals[i] = 0;
    uint64_t j = (i + 1) & ex->icnt_mask;
    while (ex->icnt_keys[j] != 0) {
        uint64_t k = ex->icnt_keys[j];
        uint32_t v = ex->icnt_vals[j];
        ex->icnt_keys[j] = 0;
        ex->icnt_vals[j] = 0;
        icnt_put(ex, k, v);
        j = (j + 1) & ex->icnt_mask;
    }
}

static int icnt_init(struct efs_export *ex, uint64_t n_hint)
{
    uint64_t cap = 16;
    while (cap < n_hint * 2)
        cap *= 2;
    free(ex->icnt_keys);
    free(ex->icnt_vals);
    ex->icnt_keys = calloc(cap, sizeof(uint64_t));
    ex->icnt_vals = calloc(cap, sizeof(uint32_t));
    if (!ex->icnt_keys || !ex->icnt_vals) {
        free(ex->icnt_keys);
        free(ex->icnt_vals);
        ex->icnt_keys = NULL;
        ex->icnt_vals = NULL;
        ex->icnt_mask = 0;
        return -1;
    }
    ex->icnt_mask = cap - 1;
    return 0;
}

static void icnt_free(struct efs_export *ex)
{
    free(ex->icnt_keys);
    free(ex->icnt_vals);
    ex->icnt_keys = NULL;
    ex->icnt_vals = NULL;
    ex->icnt_mask = 0;
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

/* Drop ino==0 table holes (torn/unflushed meta pages). Safe only when
 * child vecs will be rebuilt afterwards — do not call on a live index grow. */
static void export_drop_zero_inodes(struct efs_export *ex)
{
    uint64_t w = 0;
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (ex->inodes[i].ino == 0)
            continue;
        if (w != i)
            ex->inodes[w] = ex->inodes[i];
        w++;
    }
    if (w != ex->inode_count) {
        ex->inode_count = w;
        ex->layout_epoch++;
        efs_export_flush_mark_full(ex);
    }
}

/* Size hash tables for capacity, not live count, so a create burst does
 * not immediately rehash. */
static uint64_t inode_idx_hint(const struct efs_export *ex)
{
    uint64_t n = ex->inode_count ? ex->inode_count : 16;
    if (ex->inode_capacity > n)
        n = ex->inode_capacity;
    return n;
}

static uint64_t chunk_idx_hint(const struct efs_export *ex)
{
    uint64_t n = ex->chunk_count ? ex->chunk_count : 16;
    if (ex->chunk_capacity > n)
        n = ex->chunk_capacity;
    return n;
}

static int export_reindex_inodes(struct efs_export *ex)
{
    if (idx_init(&ex->ino_keys, &ex->ino_vals, &ex->ino_mask,
                 inode_idx_hint(ex)) != 0)
        return -1;
    if (idx_init(&ex->name_keys, &ex->name_vals, &ex->name_mask,
                 inode_idx_hint(ex)) != 0)
        return -1;
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (ex->inodes[i].ino == 0)
            continue;
        idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, ex->inodes[i].ino, i);
        name_idx_put(ex, ex->inodes[i].parent, ex->inodes[i].name, i);
    }
    return 0;
}

static int export_reindex_chunks(struct efs_export *ex)
{
    if (idx_init(&ex->chunk_keys, &ex->chunk_vals, &ex->chunk_mask,
                 chunk_idx_hint(ex)) != 0)
        return -1;
    /* Rebuild the per-ino chunk count alongside chunk_idx (same headroom). */
    if (icnt_init(ex, chunk_idx_hint(ex)) != 0)
        return -1;
    for (uint64_t i = 0; i < ex->chunk_count; i++) {
        chunk_idx_put(ex, ex->chunks[i].ino, ex->chunks[i].chunk_index, i);
        icnt_inc(ex, ex->chunks[i].ino);
    }
    return 0;
}

static int export_reindex(struct efs_export *ex)
{
    if (export_reindex_inodes(ex) != 0)
        return -1;
    return export_reindex_chunks(ex);
}

static int export_ensure_inode_idx(struct efs_export *ex)
{
    if (ex->ino_keys && ex->inode_count * 2 <= ex->ino_mask + 1)
        return 0;
    return export_reindex_inodes(ex);
}

static int export_ensure_chunk_idx(struct efs_export *ex)
{
    if (ex->chunk_keys && ex->chunk_count * 2 <= ex->chunk_mask + 1)
        return 0;
    return export_reindex_chunks(ex);
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

void efs_export_flush_mark_full(struct efs_export *ex)
{
    if (ex)
        ex->flush_full = 1;
}

void efs_export_flush_clear_dirty(struct efs_export *ex)
{
    if (!ex)
        return;
    memset(ex->flush_page_dirty, 0, sizeof(ex->flush_page_dirty));
    ex->flush_full = 0;
}

static void flush_mark_page(struct efs_export *ex, uint32_t packed)
{
    if (!ex || ex->flush_full)
        return;
    if (packed >= EFS_META_MAX_PAGES) {
        ex->flush_full = 1;
        return;
    }
    ex->flush_page_dirty[packed / 8] |= (uint8_t)(1u << (packed % 8));
}

int efs_export_flush_page_is_dirty(const struct efs_export *ex, uint32_t packed)
{
    if (!ex || ex->flush_full || packed >= EFS_META_MAX_PAGES)
        return 1;
    return (ex->flush_page_dirty[packed / 8] >> (packed % 8)) & 1;
}

void efs_export_flush_mark_ino_slot(struct efs_export *ex, uint64_t slot)
{
    if (!ex || ex->flush_full)
        return;
    uint64_t off = (uint64_t)EFS_META_HDR_SIZE +
                   slot * (uint64_t)EFS_INODE_COMPACT_SIZE;
    uint64_t end = off + (uint64_t)EFS_INODE_COMPACT_SIZE - 1;
    flush_mark_page(ex, (uint32_t)(off / EFS_META_PAGE_SIZE));
    flush_mark_page(ex, (uint32_t)(end / EFS_META_PAGE_SIZE));
}

void efs_export_flush_mark_chunk_slot(struct efs_export *ex, uint64_t slot)
{
    if (!ex || ex->flush_full)
        return;
    uint32_t ino_pc = 0;
    efs_export_meta_page_usage(ex, &ino_pc, NULL);
    uint64_t off = slot * (uint64_t)EFS_CHUNK_WIRE_SIZE;
    uint64_t end = off + (uint64_t)EFS_CHUNK_WIRE_SIZE - 1;
    flush_mark_page(ex, ino_pc + (uint32_t)(off / EFS_META_PAGE_SIZE));
    flush_mark_page(ex, ino_pc + (uint32_t)(end / EFS_META_PAGE_SIZE));
    flush_mark_page(ex, 0); /* header chunk_count */
}

void efs_export_flush_mark_dentry_tail(struct efs_export *ex)
{
    if (!ex || ex->flush_full)
        return;
    size_t dent_off = efs_meta_dent_off(EFS_META_VERSION, ex->inode_count);
    size_t dent_end = dent_off + (size_t)ex->dentry_bytes;
    if (dent_end <= dent_off) {
        flush_mark_page(ex, (uint32_t)(dent_off / EFS_META_PAGE_SIZE));
        return;
    }
    uint32_t last = (uint32_t)((dent_end - 1) / EFS_META_PAGE_SIZE);
    flush_mark_page(ex, last);
    if (last > 0)
        flush_mark_page(ex, last - 1);
    flush_mark_page(ex, 0); /* header inode_count */
}

void efs_export_flush_mark_dentry_all(struct efs_export *ex)
{
    if (!ex || ex->flush_full)
        return;
    size_t dent_off = efs_meta_dent_off(EFS_META_VERSION, ex->inode_count);
    size_t dent_end = dent_off + (size_t)ex->dentry_bytes;
    if (dent_end <= dent_off)
        dent_end = dent_off + 1;
    uint32_t p0 = (uint32_t)(dent_off / EFS_META_PAGE_SIZE);
    uint32_t p1 = (uint32_t)((dent_end - 1) / EFS_META_PAGE_SIZE);
    for (uint32_t p = p0; p <= p1; p++)
        flush_mark_page(ex, p);
}

static void flush_mark_ptr(struct efs_export *ex, const struct efs_inode *p)
{
    if (!ex || !p || !ex->inodes)
        return;
    if (p < ex->inodes)
        return;
    uint64_t slot = (uint64_t)(p - ex->inodes);
    if (slot < ex->inode_count)
        efs_export_flush_mark_ino_slot(ex, slot);
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

/* idx_init() frees the destination arrays — detach first, rehash, then free. */
static int child_idx_rehash(struct efs_export *ex, uint64_t n_hint)
{
    uint64_t old_mask = ex->child_mask;
    uint64_t *ok = ex->child_keys;
    uint64_t *ov = ex->child_vals;
    ex->child_keys = NULL;
    ex->child_vals = NULL;
    ex->child_mask = 0;
    if (idx_init(&ex->child_keys, &ex->child_vals, &ex->child_mask, n_hint) != 0) {
        ex->child_keys = ok;
        ex->child_vals = ov;
        ex->child_mask = old_mask;
        return -1;
    }
    if (ok) {
        for (uint64_t i = 0; i <= old_mask; i++) {
            if (ok[i])
                idx_put(ex->child_keys, ex->child_vals, ex->child_mask, ok[i], ov[i]);
        }
        free(ok);
        free(ov);
    }
    return 0;
}

static int export_reserve_child_vecs(struct efs_export *ex, uint64_t extra)
{
    uint64_t need = ex->child_vec_count + (extra ? extra : 1);
    if (ex->child_vec_cap < need) {
        uint64_t ncap = ex->child_vec_cap ? ex->child_vec_cap : 16;
        while (ncap < need)
            ncap *= 2;
        struct efs_child_vec *n = realloc(ex->child_vecs, ncap * sizeof(*n));
        if (!n)
            return EFS_ERR_NOMEM;
        if (ncap > ex->child_vec_cap)
            memset(n + ex->child_vec_cap, 0,
                   (ncap - ex->child_vec_cap) * sizeof(*n));
        ex->child_vecs = n;
        ex->child_vec_cap = ncap;
    }
    if (!ex->child_keys || need * 2 > ex->child_mask) {
        if (child_idx_rehash(ex, need) != 0)
            return EFS_ERR_NOMEM;
    }
    return EFS_OK;
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
        if (ncap > ex->child_vec_cap)
            memset(n + ex->child_vec_cap, 0,
                   (ncap - ex->child_vec_cap) * sizeof(*n));
        ex->child_vecs = n;
        ex->child_vec_cap = ncap;
    }
    /* Grow open-addressing table if load is high.
     * idx_init() frees the key/val arrays — detach old pointers first so
     * we can rehash from them, then free once (avoid double-free). */
    if (ex->child_vec_count * 2 > ex->child_mask) {
        if (child_idx_rehash(ex, ex->child_vec_count + 1) != 0)
            return NULL;
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
        if (ex->inodes[i].ino == 0)
            continue;
        if (ex->inodes[i].ino == EFS_ROOT_INO &&
            ex->inodes[i].parent == EFS_ROOT_INO)
            continue;
        if (!ex->inodes[i].name[0] || ex->inodes[i].parent == 0)
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
        /* ino==0 rows are not in the name index (lookup → ENOENT) but used
         * to appear in readdir — walkers then fstatat a listed name and fail. */
        if (ex->inodes[slot].ino == 0)
            continue;
        if (ex->inodes[slot].ino == parent)
            continue;
        if (ex->inodes[slot].parent != parent)
            continue;
        uint64_t npos = 0;
        if (name_idx_get(ex, parent, ex->inodes[slot].name, &npos) != 0 ||
            npos != slot)
            continue;
        int rc = cb(ex, slot, arg);
        if (rc != 0)
            return rc;
    }
    return EFS_OK;
}

static int dir_nonempty_cb(struct efs_export *ex, uint64_t slot, void *arg)
{
    (void)ex;
    (void)slot;
    (void)arg;
    return 1;  /* any child terminates the walk with a non-zero rc */
}

/* Return 1 if the directory has no children, 0 otherwise. */
int efs_export_dir_empty(struct efs_export *ex, efs_ino_t ino)
{
    return efs_export_foreach_child(ex, ino, dir_nonempty_cb, NULL) == 0;
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
    flush_mark_ptr(ex, d);
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
    flush_mark_ptr(ex, p);
    efs_ino_t a = p->parent;
    efs_ino_t prev = parent;
    while (a != prev) {
        struct efs_inode *ap = inode_ptr(ex, a);
        if (!ap || !efs_mode_is_dir(ap->mode))
            break;
        ancestor_add_tree(ap, tf, td, tb, tlo, thi);
        flush_mark_ptr(ex, ap);
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
    flush_mark_ptr(ex, p);

    efs_ino_t a = p->parent;
    efs_ino_t prev = parent;
    while (a != prev) {
        struct efs_inode *ap = inode_ptr(ex, a);
        if (!ap || !efs_mode_is_dir(ap->mode))
            break;
        ancestor_sub_tree(ap, tf, td, tb);
        flush_mark_ptr(ex, ap);
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
        flush_mark_ptr(ex, d);
        if (d->parent == cur)
            break;
        prev = cur;
        cur = d->parent;
    }
}

/* Incremental time rollup for "time moved forward" mutations (create/write/
 * chmod/chown/utimens all stamp ctime = now, and entry_tmax is dominated by
 * ctime). The child's entry range can only widen an ancestor's [tmin,tmax],
 * never narrow it, so expand in O(depth) instead of the O(children) rescan in
 * recompute_times_up. Without this, every write in a growing directory cost
 * O(dir_children) → O(n^2) bulk copies. The narrowing case (unlink) still does
 * a full rescan via rollup_sub_under → recompute_times_up. */
static void expand_parent_chain(struct efs_export *ex, efs_ino_t from_ino,
                                efs_ino_t parent, uint64_t lo, uint64_t hi)
{
    efs_ino_t cur = parent;
    efs_ino_t prev = from_ino;
    int first = 1;
    while (cur != prev) {
        struct efs_inode *d = inode_ptr(ex, cur);
        if (!d || !efs_mode_is_dir(d->mode))
            break;
        if (first) { /* immediate parent also tracks imm_* */
            int has = (d->imm_files + d->imm_dirs) > 0;
            times_expand(&d->imm_tmin, &d->imm_tmax, lo, hi, &has);
            first = 0;
        }
        int hast = (d->tree_files + d->tree_dirs) > 0;
        times_expand(&d->tree_tmin, &d->tree_tmax, lo, hi, &hast);
        flush_mark_ptr(ex, d);
        if (d->parent == cur)
            break;
        prev = cur;
        cur = d->parent;
    }
}

static void rollup_expand_parents_of(struct efs_export *ex, efs_ino_t ino)
{
    struct efs_inode *p = inode_ptr(ex, ino);
    if (!p)
        return;
    uint64_t lo = entry_tmin(p), hi = entry_tmax(p);
    /* Directories always have nlink>=2 (".", subdirs) but a single inode row —
     * not hardlinks. Take the O(depth) single-parent walk for them; only scan
     * all rows for genuinely hardlinked multi-link files (rare). */
    if (p->nlink <= 1 || efs_mode_is_dir(p->mode)) {
        expand_parent_chain(ex, ino, p->parent, lo, hi);
        return;
    }
    /* Hardlinked: one inode row per link, each with its own parent (rare). */
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (ex->inodes[i].ino == ino)
            expand_parent_chain(ex, ino, ex->inodes[i].parent, lo, hi);
    }
}

static void recompute_dir_postorder(struct efs_export *ex, efs_ino_t dir_ino,
                                    uint8_t *vis)
{
    struct efs_inode *d = inode_ptr(ex, dir_ino);
    if (!d || !efs_mode_is_dir(d->mode))
        return;
    /* A torn/holed meta page can scramble parent pointers into a directory
     * cycle (a dir that is its own ancestor). Recursing on it overflowed the
     * catch-up thread's stack — the "raced deserialize SIGSEGV". Mark visited
     * dir slots and bail on a revisit so a garbage table cannot recurse
     * forever; the cycle's rollups stay stale until a clean flush re-publishes
     * the subtree. */
    size_t dslot = (size_t)(d - ex->inodes);
    if (dslot < ex->inode_count) {
        if (vis[dslot])
            return;
        vis[dslot] = 1;
    }
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
            recompute_dir_postorder(ex, c->ino, vis);
        parent_add_child(d, c);
    }
}

static void pending_rollup_clear(struct efs_export *ex)
{
    ex->pending_rollup_count = 0;
    ex->rollups_stale = 0;
}

static int pending_rollup_note(struct efs_export *ex, efs_ino_t ino,
                               int64_t size_delta, int touch)
{
    for (uint64_t i = 0; i < ex->pending_rollup_count; i++) {
        if (ex->pending_rollup_inos[i] == ino) {
            ex->pending_rollup_deltas[i] += size_delta;
            if (touch)
                ex->pending_rollup_touch[i] = 1;
            return 0;
        }
    }
    if (ex->pending_rollup_count >= ex->pending_rollup_cap) {
        uint64_t ncap = ex->pending_rollup_cap ? ex->pending_rollup_cap * 2 : 64;
        efs_ino_t *ninos = realloc(ex->pending_rollup_inos, ncap * sizeof(*ninos));
        int64_t *ndeltas = realloc(ex->pending_rollup_deltas, ncap * sizeof(*ndeltas));
        uint8_t *ntouch = realloc(ex->pending_rollup_touch, ncap * sizeof(*ntouch));
        if (!ninos || !ndeltas || !ntouch) {
            free(ninos);
            free(ndeltas);
            free(ntouch);
            return -1;
        }
        ex->pending_rollup_inos = ninos;
        ex->pending_rollup_deltas = ndeltas;
        ex->pending_rollup_touch = ntouch;
        ex->pending_rollup_cap = ncap;
    }
    uint64_t i = ex->pending_rollup_count++;
    ex->pending_rollup_inos[i] = ino;
    ex->pending_rollup_deltas[i] = size_delta;
    ex->pending_rollup_touch[i] = touch ? 1 : 0;
    return 0;
}

static void apply_size_delta_for_ino(struct efs_export *ex, efs_ino_t ino,
                                     int64_t delta)
{
    if (delta == 0)
        return;
    struct efs_inode *p = inode_ptr(ex, ino);
    if (!p || efs_mode_is_dir(p->mode))
        return;
    if (p->nlink <= 1) {
        rollup_size_delta(ex, p->parent, delta);
        return;
    }
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (ex->inodes[i].ino == ino)
            rollup_size_delta(ex, ex->inodes[i].parent, delta);
    }
}

void efs_export_recompute_rollups(struct efs_export *ex)
{
    if (!ex)
        return;
    efs_export_flush_mark_full(ex);
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (efs_mode_is_dir(ex->inodes[i].mode))
            inode_clear_rollups(&ex->inodes[i]);
        else
            inode_clear_rollups(&ex->inodes[i]);
    }
    child_idx_rebuild(ex);
    /* Visited-set for cycle-safe postorder (see recompute_dir_postorder). On
     * allocation failure skip the recompute rather than risk the recursion —
     * rollups stay cleared (stale) instead of crashing the catch-up thread. */
    uint8_t *vis = calloc(ex->inode_count ? ex->inode_count : 1, 1);
    if (vis) {
        recompute_dir_postorder(ex, EFS_ROOT_INO, vis);
        free(vis);
    }
    pending_rollup_clear(ex);
}

void efs_export_ensure_rollups(struct efs_export *ex)
{
    if (!ex || !ex->rollups_stale)
        return;
    for (uint64_t i = 0; i < ex->pending_rollup_count; i++) {
        efs_ino_t ino = ex->pending_rollup_inos[i];
        int64_t delta = ex->pending_rollup_deltas[i];
        uint8_t flags = ex->pending_rollup_touch[i];
        if (flags & EFS_ROLLUP_CREATE) {
            struct efs_inode *c = inode_ptr(ex, ino);
            if (c)
                rollup_add_under(ex, c->parent, c);
            if (delta != 0)
                apply_size_delta_for_ino(ex, ino, delta);
        } else {
            apply_size_delta_for_ino(ex, ino, delta);
            if (delta != 0 || (flags & EFS_ROLLUP_TOUCH))
                rollup_expand_parents_of(ex, ino);
        }
    }
    pending_rollup_clear(ex);
}

int efs_export_format_stats_ex(const struct efs_export *ex,
                               const struct efs_inode *dir, char *buf,
                               size_t buflen)
{
    if (!dir || !buf || buflen == 0 || !efs_mode_is_dir(dir->mode))
        return -1;
    uint32_t ino_pg = 0, ch_pg = 0;
    if (ex)
        efs_export_meta_page_usage(ex, &ino_pg, &ch_pg);
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
                     "tree_tmax=%llu\n"
                     "meta_ino_pages=%u\n"
                     "meta_ino_pages_max=%u\n"
                     "meta_chunk_pages=%u\n"
                     "meta_chunk_pages_max=%u\n",
                     (unsigned long long)dir->imm_files,
                     (unsigned long long)dir->imm_dirs,
                     (unsigned long long)dir->imm_bytes,
                     (unsigned long long)dir->imm_tmin,
                     (unsigned long long)dir->imm_tmax,
                     (unsigned long long)dir->tree_files,
                     (unsigned long long)dir->tree_dirs,
                     (unsigned long long)dir->tree_bytes,
                     (unsigned long long)dir->tree_tmin,
                     (unsigned long long)dir->tree_tmax,
                     ino_pg, (unsigned)EFS_META_INO_PAGE_MAX,
                     ch_pg, (unsigned)EFS_META_CHUNK_PAGE_MAX);
    if (n < 0 || (size_t)n >= buflen)
        return -1;
    return n;
}

int efs_export_format_stats(const struct efs_inode *dir, char *buf, size_t buflen)
{
    return efs_export_format_stats_ex(NULL, dir, buf, buflen);
}

/* Copy shared inode fields onto every hard-link row with the same ino.
 * Parent/name of each directory entry are preserved. */
static void sync_hardlink_attrs(struct efs_export *ex, efs_ino_t ino,
                                const struct efs_inode *src)
{
    /* Single link: attrs already live on the indexed row (inode_ptr).
     * Directories have nlink>=2 without hardlink rows — nothing to sync. */
    if (!src || src->nlink <= 1 || efs_mode_is_dir(src->mode))
        return;
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
        efs_export_flush_mark_ino_slot(ex, i);
    }
}

/* Swap-remove chunks[j]; caller already bumped layout_epoch. */
static void remove_chunk_at(struct efs_export *ex, uint64_t j)
{
    ex->chunk_epoch++;
    uint32_t cidx = ex->chunks[j].chunk_index;
    efs_ino_t ino = ex->chunks[j].ino;
    chunk_idx_del(ex, ino, cidx);
    icnt_dec(ex, ino);
    uint64_t clast = ex->chunk_count - 1;
    if (j != clast) {
        chunk_idx_del(ex, ex->chunks[clast].ino, ex->chunks[clast].chunk_index);
        ex->chunks[j] = ex->chunks[clast];
        ex->chunk_count--;
        chunk_idx_put(ex, ex->chunks[j].ino, ex->chunks[j].chunk_index, j);
    } else {
        ex->chunk_count--;
    }
}

/* Scan this table's chunk array. After extent sharding a single table
 * only holds some groups, so a sequential ci walk stops at the first
 * hole and leaks the rest. */
static void drop_chunks_scan(struct efs_export *ex, efs_ino_t ino,
                             uint32_t first_chunk)
{
    /* efs_export_drop_chunks_from fans this over the main table AND every
     * loaded shard tab, but an ino's chunks live in only one or a few of them.
     * For every other table icnt_get() is 0, the fast path below is skipped
     * (it requires total > 0) and the fallback scans that unrelated table's
     * entire chunk array -- so one unlink cost O(all chunks on this node).
     * That is the term behind unlink p50 growing 0.44ms -> 4.6ms between 100k
     * and 5M inodes. icnt is maintained everywhere chunk_idx is (set_chunk,
     * merge, remove_chunk_at, export_reindex_chunks), so a zero is exactly as
     * trustworthy as the count the fast path already returns on. Checked
     * before layout_epoch++: a table with nothing to drop did not change, and
     * a spurious bump makes the flush treat the table as raced and stay dirty.
     * Holds for first_chunk > 0 too -- no chunks means none in any range. */
    if (ex->icnt_keys && ex->icnt_mask != 0 && icnt_get(ex, ino) == 0)
        return;
    ex->layout_epoch++;
    /* Fast path: probe by chunk_index via chunk_idx, bounded by the per-ino
     * live count, instead of scanning the whole chunk array under the
     * metadata lock. Only valid for a full drop (first_chunk==0): the count
     * is the exact number of this ino's chunks in this table, so finding
     * `total` of them means none are left. A partial truncate (first_chunk>0)
     * has no exact per-range count, and a missing or stale count falls
     * through to the authoritative scan below. */
    uint64_t removed = 0;
    if (first_chunk == 0) {
        uint32_t total = icnt_get(ex, ino);
        if (total > 0) {
            uint32_t found = 0;
            /* Generous bound: a dense file needs `total` probes, a sparse one
             * more. On exhaustion (stale/inflated count or extreme layout)
             * fall through to the scan. */
            uint64_t limit = (uint64_t)total * 4 + 4096;
            for (uint64_t ci = 0; found < total && ci < limit; ci++) {
                uint64_t pos = 0;
                if (chunk_idx_get(ex, ino, (uint32_t)ci, &pos) == 0) {
                    remove_chunk_at(ex, pos);
                    found++;
                }
            }
            removed += found;
            if (found == total) {
                if (removed) {
                    ex->shard_dirty = 1;
                    efs_export_flush_mark_full(ex);
                }
                return;
            }
        }
    }
    uint64_t i = 0;
    while (i < ex->chunk_count) {
        if (ex->chunks[i].ino == ino &&
            ex->chunks[i].chunk_index >= first_chunk) {
            remove_chunk_at(ex, i);
            removed++;
        } else {
            i++;
        }
    }
    /* Mark dirty HERE, per table, and only when this table actually lost a
     * chunk. The DROP_CHUNKS handler used to set ex->shard_dirty on the MAIN
     * table unconditionally for every fanned drop, so one unlink anywhere
     * latched every peer's main table dirty forever — nothing on a peer ever
     * clears it, because only a main-table flush does and peers do not own
     * shard 0. That permanently tripped the META_COMMIT promote gate
     * (`if (ex->shard_dirty) reply = BUSY`), which is why peers stashed every
     * prepare but stayed pinned at an old committed gen. */
    if (removed) {
        ex->shard_dirty = 1;
        efs_export_flush_mark_full(ex);
    }
}

static void remove_chunks_for_ino(struct efs_export *ex, efs_ino_t ino)
{
    efs_export_drop_chunks_from(ex, ino, 0);
}

void efs_export_drop_chunks_from(struct efs_export *ex, efs_ino_t ino,
                                 uint32_t first_chunk)
{
    if (!ex)
        return;
    if (export_is_sharded_root(ex) && ex->root.shard_count > 1) {
        drop_chunks_scan(ex, ino, first_chunk);
        if (ex->shard_tabs) {
            for (uint32_t s = 1; s < ex->shard_tab_cap; s++) {
                if (ex->shard_tabs[s])
                    drop_chunks_scan(ex->shard_tabs[s], ino, first_chunk);
            }
        }
        return;
    }
    drop_chunks_scan(ex, ino, first_chunk);
}

void efs_export_drop_chunks_table(struct efs_export *tab, efs_ino_t ino,
                                  uint32_t first_chunk)
{
    if (tab)
        drop_chunks_scan(tab, ino, first_chunk);
}

/* Swap-remove inode array slot i; refresh indexes for the moved row.
 * Caller must already have applied rollup_sub and child_idx_del for slot i.
 * expect_survivor: another hard-link row for this ino remains (nlink > 0). */
static void remove_inode_slot(struct efs_export *ex, uint64_t i, int expect_survivor)
{
    efs_ino_t old_parent = ex->inodes[i].parent;
    char old_name[EFS_MAX_NAME];
    memcpy(old_name, ex->inodes[i].name, EFS_MAX_NAME);
    efs_ino_t old_ino = ex->inodes[i].ino;

    name_idx_del(ex, old_parent, old_name);
    dentry_bytes_sub(ex, old_name);

    ex->layout_epoch++;
    efs_export_flush_mark_full(ex);
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

    /* ino_idx holds one slot per ino. If it still points at a live row with
     * old_ino (hard-link survivor, or the moved last row was the same ino),
     * leave it. Otherwise drop it — do not scan the inode table. */
    uint64_t pos = 0;
    if (ex->ino_keys &&
        idx_get(ex->ino_keys, ex->ino_vals, ex->ino_mask, old_ino, &pos) == 0 &&
        pos < ex->inode_count && ex->inodes[pos].ino == old_ino)
        return;

    if (!expect_survivor) {
        idx_del(ex->ino_keys, ex->ino_vals, ex->ino_mask, old_ino);
        return;
    }

    /* Rare: unlinked the indexed hard-link row; find another dent. */
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
    ex->chunk_size = EFS_DEFAULT_CHUNK_SIZE;
    ex->features = EFS_FEATURES_DEFAULT;
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
    dentry_bytes_add(ex, root.name);
    export_reindex(ex);
    child_idx_rebuild(ex);
    ex->efsm_version = EFS_META_EFSM_V6;
    ex->root.shard_count = 1;
    ex->root.shard_bits = 0;
}

void efs_export_free(struct efs_export *ex)
{
    if (!ex)
        return;
    if (ex->shard_tabs) {
        for (uint32_t i = 0; i < ex->shard_tab_cap; i++) {
            if (!ex->shard_tabs[i])
                continue;
            efs_export_free(ex->shard_tabs[i]);
            free(ex->shard_tabs[i]);
        }
        free(ex->shard_tabs);
        ex->shard_tabs = NULL;
        ex->shard_tab_cap = 0;
    }
    free(ex->inodes);
    free(ex->chunks);
    free(ex->pending_rollup_inos);
    free(ex->pending_rollup_deltas);
    free(ex->pending_rollup_touch);
    idx_free(&ex->ino_keys, &ex->ino_vals, &ex->ino_mask);
    idx_free(&ex->name_keys, &ex->name_vals, &ex->name_mask);
    idx_free(&ex->chunk_keys, &ex->chunk_vals, &ex->chunk_mask);
    icnt_free(ex);
    child_vecs_free(ex);
    efs_export_root_free(&ex->root);
    free(ex->gm_blob);
    free(ex->flush_blob);
    memset(ex, 0, sizeof(*ex));
}

static void efs_export_init_empty_table(struct efs_export *ex, efs_export_id_t id,
                                        const char *name, uint32_t shard_bits,
                                        uint32_t shard_count, uint32_t shard)
{
    memset(ex, 0, sizeof(*ex));
    ex->id = id;
    if (name)
        strncpy(ex->name, name, EFS_MAX_NAME - 1);
    ex->chunk_size = EFS_DEFAULT_CHUNK_SIZE;
    ex->features = EFS_FEATURES_DEFAULT;
    ex->inode_capacity = 16;
    ex->inodes = calloc(ex->inode_capacity, sizeof(struct efs_inode));
    ex->chunk_capacity = 16;
    ex->chunks = calloc(ex->chunk_capacity, sizeof(struct efs_chunk_entry));
    ex->efsm_version = EFS_META_VERSION;
    ex->root.shard_count = shard_count;
    ex->root.shard_bits = shard_bits;
    if (shard_bits)
        ex->next_ino = shard; /* low-bits: this shard's congruence-class base */
    export_reindex(ex);
    child_idx_rebuild(ex);
}

/* Get or lazily create the shard table for `shard`: grow the shard_tabs
 * array, calloc + init the table, and reload a previously flushed shard root
 * from the cluster root's extra_roots (this is the on-demand shard load).
 * This is a PER-EXPORT mutation — call only under the global lock or while
 * holding all the export's shard locks (rebuild). Blocker 2 pre-creates every
 * table at rehash/rebuild so op/read paths never reach this under a single
 * shard lock. */
static struct efs_export *shard_tab_get_or_create(struct efs_export *ex,
                                                  uint32_t shard)
{
    uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
    if (!ex->shard_tabs || ex->shard_tab_cap < sc) {
        struct efs_export **n = calloc(sc, sizeof(*n));
        if (!n)
            return NULL;
        if (ex->shard_tabs) {
            uint32_t old = ex->shard_tab_cap < sc ? ex->shard_tab_cap : sc;
            memcpy(n, ex->shard_tabs, (size_t)old * sizeof(*n));
            free(ex->shard_tabs);
        }
        ex->shard_tabs = n;
        ex->shard_tab_cap = sc;
    }
    if (!ex->shard_tabs[shard]) {
        struct efs_export *tab = calloc(1, sizeof(*tab));
        if (!tab)
            return NULL;
        efs_export_init_empty_table(tab, ex->id, ex->name, ex->root.shard_bits,
                                    sc, shard);
        tab->chunk_size = ex->chunk_size;
        tab->features = ex->features;
        tab->shard_id = shard;
        /* Reload a previously flushed extra from the cluster root. */
        for (uint32_t i = 0; i < ex->root.extra_shard_count; i++) {
            if (ex->root.extra_shard_ids && ex->root.extra_shard_ids[i] == shard &&
                ex->root.extra_roots) {
                if (efs_export_root_copy(&tab->root, &ex->root.extra_roots[i]) ==
                    EFS_OK) {
                    tab->meta_fragmented = 1;
                    tab->meta_needs_rebuild = (tab->root.page_count > 0);
                    tab->next_ino = tab->root.next_ino;
                }
                break;
            }
        }
        ex->shard_tabs[shard] = tab;
    }
    return ex->shard_tabs[shard];
}

struct efs_export *efs_export_table(struct efs_export *ex, uint32_t shard)
{
    if (!ex)
        return NULL;
    uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
    if (shard == 0 || ex->root.shard_bits == 0 || sc <= 1)
        return ex;
    if (shard >= sc || shard >= EFS_META_MAX_SHARDS)
        return NULL;
    struct efs_export *tab = shard_tab_get_or_create(ex, shard);
    if (!tab)
        return NULL;
    /* LRU tick. Atomic so a (future) shard-lock-holding read path can bump it
     * without the global lock; the value only guides cold-shard eviction. */
    tab->shard_tick = __atomic_add_fetch(&ex->shard_tick, 1, __ATOMIC_RELAXED);
    return tab;
}

/* Blocker 2: pre-create every shard table so op/read paths find an existing
 * table and never do the per-export lazy-create mutation under a single shard
 * lock. Caller holds the global lock (rehash) or all the export's shard locks
 * (rebuild). Cheap: non-owned tables stay empty/hollow until their owner
 * rebuilds them. */
int efs_export_precreate_shards(struct efs_export *ex)
{
    if (!ex || ex->root.shard_bits == 0)
        return EFS_OK;
    uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
    if (sc > EFS_META_MAX_SHARDS)
        sc = EFS_META_MAX_SHARDS;
    for (uint32_t shard = 1; shard < sc; shard++)
        if (!shard_tab_get_or_create(ex, shard))
            return EFS_ERR_NOMEM;
    return EFS_OK;
}

struct efs_export *efs_export_table_for_ino(struct efs_export *ex, efs_ino_t ino)
{
    if (!ex)
        return NULL;
    if (ex->root.shard_bits == 0)
        return ex;
    return efs_export_table(ex, efs_export_shard_of(ino, ex->root.shard_bits));
}

struct efs_export *efs_export_shard_tab(struct efs_export *ex, uint32_t shard)
{
    if (!ex)
        return NULL;
    if (shard == 0 || ex->root.shard_bits == 0 ||
        (ex->root.shard_count ? ex->root.shard_count : 1) <= 1)
        return ex;
    if (!ex->shard_tabs || shard >= ex->shard_tab_cap)
        return NULL;
    return ex->shard_tabs[shard];
}

void efs_export_install_extra_roots(struct efs_export *ex)
{
    /* Pages + the v8 extra_roots descriptors are the database. Instantiating
     * every extra table here forced every peer to rebuild every shard after
     * every extras PUT_META (the create/unlink heal storm). The owner
     * catchup loads a shard when this node owns it. */
    (void)ex;
}

int efs_export_root_same_pages(const struct efs_export_root *a,
                               const struct efs_export_root *b)
{
    if (!a || !b || a->page_count == 0 || a->page_count != b->page_count)
        return 0;
    if (!a->page_checksums || !b->page_checksums)
        return 0;
    return memcmp(a->page_checksums, b->page_checksums,
                  (size_t)a->page_count * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE) == 0;
}

void efs_export_merge_extra_roots(struct efs_export *ex,
                                  const struct efs_export_root *incoming)
{
    if (!ex || !incoming || !incoming->extra_shard_count ||
        !incoming->extra_roots)
        return;
    for (uint32_t i = 0; i < incoming->extra_shard_count; i++) {
        uint32_t sh = incoming->extra_shard_ids ? incoming->extra_shard_ids[i]
                                                : 0;
        if (sh == 0)
            continue;
        const struct efs_export_root *desc = &incoming->extra_roots[i];
        struct efs_export *tab = efs_export_shard_tab(ex, sh);
        if (!tab || tab == ex)
            continue;
        /* Never fence a shard table with unflushed ops: its in-memory state
         * is ahead of any descriptor a peer can propagate (descriptors are
         * written by flushing a snapshot that predates those ops). */
        if (tab->shard_dirty || tab->meta_needs_rebuild)
            continue;
        /* Single writer per shard (its owner) => equal/higher local gen is
         * authoritative-or-equal content. Skip: installing an older/equal
         * descriptor would regress next_ino and trigger a pointless rebuild
         * of pages we already have (or, worse, pages GC'd after our newer
         * flush superseded them). */
        if (tab->meta_fragmented && tab->root.page_count > 0 &&
            tab->root.generation >= desc->generation)
            continue;
        /* Stale materialized copy: drop it. Do not mark needs_rebuild —
         * that made every peer reassemble the shard. Pages + descriptor
         * are enough; the owner reloads on demand. */
        efs_export_free(tab);
        free(tab);
        ex->shard_tabs[sh] = NULL;
    }
    /* Monotonic union. Capture-then-maxmerge used to wipe descriptors this
     * node has no local table for (owner-only RAM) and then restore only
     * THIS incoming's extras — extra-owner PUT_META is filtered to the
     * sender's shards, so each refresh orphaned everyone else's recovery
     * record and catchup oscillated 5↔7. */
    (void)efs_export_root_maxmerge_extras(&ex->root, incoming);
}

static int export_is_sharded_root(struct efs_export *ex)
{
    return ex && ex->root.shard_bits && ex->root.shard_count > 1 &&
           inode_ptr(ex, EFS_ROOT_INO) != NULL;
}

int efs_export_load_shard(struct efs_export *ex, uint32_t shard)
{
    if (!ex)
        return EFS_ERR_INVAL;
    if (shard == 0 || ex->root.shard_bits == 0)
        return EFS_OK;
    return efs_export_table(ex, shard) ? EFS_OK : EFS_ERR_NOMEM;
}

void efs_export_evict_cold_shards(struct efs_export *ex, uint32_t keep)
{
    if (!ex || !ex->shard_tabs || keep == 0)
        return;
    uint32_t n = 0;
    for (uint32_t i = 1; i < ex->shard_tab_cap; i++)
        if (ex->shard_tabs[i])
            n++;
    while (n > keep) {
        uint32_t victim = 0;
        uint64_t oldest = UINT64_MAX;
        for (uint32_t i = 1; i < ex->shard_tab_cap; i++) {
            struct efs_export *tab = ex->shard_tabs[i];
            if (!tab || tab->shard_dirty || tab->meta_needs_rebuild)
                continue;
            if (tab->shard_tick < oldest) {
                oldest = tab->shard_tick;
                victim = i;
            }
        }
        if (!victim)
            break;
        efs_export_free(ex->shard_tabs[victim]);
        free(ex->shard_tabs[victim]);
        ex->shard_tabs[victim] = NULL;
        n--;
    }
}

int efs_export_rehash(struct efs_export *ex, uint32_t new_bits)
{
    if (!ex)
        return EFS_ERR_INVAL;
    if (new_bits > 20)
        new_bits = 20;
    uint32_t new_sc = new_bits ? (1u << new_bits) : 1u;
    if (new_sc > EFS_META_MAX_SHARDS)
        new_sc = EFS_META_MAX_SHARDS;

    uint64_t nino = ex->inode_count;
    uint64_t nch = ex->chunk_count;
    if (ex->shard_tabs) {
        for (uint32_t i = 1; i < ex->shard_tab_cap; i++) {
            if (!ex->shard_tabs[i])
                continue;
            nino += ex->shard_tabs[i]->inode_count;
            nch += ex->shard_tabs[i]->chunk_count;
        }
    }
    struct efs_inode *inos = nino ? malloc((size_t)nino * sizeof(*inos)) : NULL;
    struct efs_chunk_entry *chs = nch ? malloc((size_t)nch * sizeof(*chs)) : NULL;
    if ((nino && !inos) || (nch && !chs)) {
        free(inos);
        free(chs);
        return EFS_ERR_NOMEM;
    }
    uint64_t wi = 0, wc = 0;
    for (uint64_t i = 0; i < ex->inode_count; i++)
        inos[wi++] = ex->inodes[i];
    for (uint64_t i = 0; i < ex->chunk_count; i++)
        chs[wc++] = ex->chunks[i];
    if (ex->shard_tabs) {
        for (uint32_t s = 1; s < ex->shard_tab_cap; s++) {
            struct efs_export *tab = ex->shard_tabs[s];
            if (!tab)
                continue;
            for (uint64_t i = 0; i < tab->inode_count; i++)
                inos[wi++] = tab->inodes[i];
            for (uint64_t i = 0; i < tab->chunk_count; i++)
                chs[wc++] = tab->chunks[i];
        }
    }

    ex->root.shard_bits = new_bits;
    ex->root.shard_count = new_sc;
    ex->root.version = EFS_META_ROOT_VERSION_V8;

    if (new_bits == 0) {
        for (uint64_t i = 0; i < wi; i++) {
            if (inos[i].ino == EFS_ROOT_INO)
                continue;
            if (inode_ptr(ex, inos[i].ino) == NULL)
                (void)efs_export_upsert_inode(ex, &inos[i]);
        }
        for (uint64_t i = 0; i < wc; i++)
            (void)efs_export_set_chunk(ex, chs[i].ino, chs[i].chunk_index,
                                       chs[i].fragment_nodes, chs[i].checksums);
        if (ex->shard_tabs) {
            for (uint32_t s = 1; s < ex->shard_tab_cap; s++) {
                if (!ex->shard_tabs[s])
                    continue;
                efs_export_free(ex->shard_tabs[s]);
                free(ex->shard_tabs[s]);
                ex->shard_tabs[s] = NULL;
            }
        }
        free(inos);
        free(chs);
        return EFS_OK;
    }

    /* Blocker 2: pre-create every shard table so later op/read paths find an
     * existing table and never do the per-export lazy-create mutation under a
     * single shard lock. */
    if (efs_export_precreate_shards(ex) != EFS_OK) {
        free(inos);
        free(chs);
        return EFS_ERR_NOMEM;
    }

    for (uint64_t i = 0; i < wi; i++) {
        if (inos[i].ino == 0)
            continue;
        uint32_t dest = efs_export_shard_of(inos[i].ino, new_bits);
        uint32_t psh = efs_export_shard_of(inos[i].parent, new_bits);
        struct efs_export *dtab = efs_export_table(ex, dest);
        if (dtab)
            (void)efs_export_upsert_inode(dtab, &inos[i]);
        if (dest != psh) {
            struct efs_export *ptab = efs_export_table(ex, psh);
            if (ptab && ptab != dtab)
                (void)efs_export_upsert_inode(ptab, &inos[i]);
        }
    }
    for (uint64_t i = 0; i < wc; i++) {
        uint32_t dest = efs_export_chunk_shard_of(chs[i].ino, chs[i].chunk_index,
                                                 new_bits);
        struct efs_export *dtab = efs_export_table(ex, dest);
        if (dtab)
            (void)efs_export_set_chunk(dtab, chs[i].ino, chs[i].chunk_index,
                                       chs[i].fragment_nodes, chs[i].checksums);
    }
    for (uint64_t i = ex->inode_count; i > 0; i--) {
        struct efs_inode *p = &ex->inodes[i - 1];
        if (p->ino == EFS_ROOT_INO)
            continue;
        uint32_t dest = efs_export_shard_of(p->ino, new_bits);
        uint32_t psh = efs_export_shard_of(p->parent, new_bits);
        if (dest != 0 && psh != 0)
            (void)efs_export_unlink(ex, p->ino);
    }
    free(inos);
    free(chs);
    return EFS_OK;
}

uint32_t efs_export_create_target(struct efs_export *ex, efs_ino_t parent,
                                  uint32_t mode, const char *name)
{
    if (!ex)
        return 0;
    uint32_t bits = ex->root.shard_bits;
    uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
    uint32_t psh = efs_export_shard_of(parent, bits);
    if (sc <= 1 || bits == 0)
        return psh;
    /* Only directories under ROOT hash. That puts an independent tree
     * (posix testdir, ecopy dest) on hash(1, name) so creates do not all
     * hit the metadata primary. Nested dirs stay on that dest shard so
     * rename/hardlink stay one-node — hashing every dir splits dentry
     * from inode and breaks rmdir-nonempty / LOOKUP / ecopy rename. */
    if (efs_mode_is_dir(mode) && name && name[0] && parent == EFS_ROOT_INO)
        return efs_export_dentry_shard_of(parent, name, bits);
    (void)mode;
    return psh;
}

static int lookup_on_tab(struct efs_export *tab, efs_ino_t parent,
                         const char *name, struct efs_inode *out)
{
    uint64_t pos = 0;
    if (!tab || !name || name_idx_get(tab, parent, name, &pos) != 0)
        return EFS_ERR_NOT_FOUND;
    if (out)
        *out = tab->inodes[pos];
    return EFS_OK;
}

static efs_ino_t create_sharded(struct efs_export *ex, efs_ino_t parent,
                                uint32_t mode, uid_t uid, gid_t gid,
                                const char *name)
{
    uint32_t bits = ex->root.shard_bits;
    uint32_t psh = efs_export_shard_of(parent, bits);
    uint32_t target = efs_export_create_target(ex, parent, mode, name);
    struct efs_export *ctab = efs_export_table(ex, target);
    if (!ctab)
        return 0;
    struct efs_export *ptab = efs_export_table(ex, psh);
    if (!ptab)
        return 0;
    /* Phase 3b: once the parent is past EFS_DIR_SPREAD_MIN the dentry
     * lands on hash(parent, name), not the parent shard. */
    uint32_t dsh = psh;
    if (efs_export_dir_is_spread(ex, parent))
        dsh = efs_export_dentry_shard_of(parent, name, bits);
    struct efs_export *dtab = efs_export_table(ex, dsh);
    if (!dtab)
        return 0;
    if (lookup_on_tab(dtab, parent, name, NULL) == EFS_OK)
        return 0;
    if (dtab != ptab && lookup_on_tab(ptab, parent, name, NULL) == EFS_OK)
        return 0;
    if (target != dsh) {
        uint64_t stale;
        if (name_idx_get(ctab, parent, name, &stale) == 0) {
            struct efs_inode victim = ctab->inodes[stale];
            rollup_sub_under(ctab, parent, &victim);
            child_idx_del(ctab, parent, stale);
            remove_inode_slot(ctab, stale, 0);
            fprintf(stderr, "create: dropped stale child-table row "
                    "parent=%llu name=%s (split-brain remnant)\n",
                    (unsigned long long)parent, name);
        }
    }
    efs_ino_t ino = efs_export_alloc_ino_for_shard(ex, target);
    if (!ino)
        return 0;
    if (!efs_export_create_with_ino(ctab, ino, parent, mode, uid, gid, name))
        return 0;
    ctab->shard_dirty = 1;
    if (dtab != ctab) {
        (void)efs_export_create_with_ino(dtab, ino, parent, mode, uid, gid,
                                         name);
        dtab->shard_dirty = 1;
    }
    /* Rollups live on the parent inode row. If neither write landed
     * on the parent table, bump it here. */
    if (ptab != ctab && ptab != dtab) {
        struct efs_inode dent;
        if (lookup_on_tab(dtab, parent, name, &dent) == EFS_OK ||
            lookup_on_tab(ctab, parent, name, &dent) == EFS_OK)
            rollup_add_under(ptab, parent, &dent);
    }
    return ino;
}

static void lookup_stitch_child(struct efs_export *ex, struct efs_inode *out)
{
    if (!ex || !out || !export_is_sharded_root(ex))
        return;
    uint32_t bits = ex->root.shard_bits;
    uint32_t csh = efs_export_shard_of(out->ino, bits);
    struct efs_export *ctab = (csh == 0) ? ex : efs_export_shard_tab(ex, csh);
    struct efs_inode full;
    if (ctab && efs_export_get_inode(ctab, out->ino, &full) == 0) {
        char nbuf[EFS_MAX_NAME];
        memcpy(nbuf, out->name, EFS_MAX_NAME);
        efs_ino_t p = out->parent;
        *out = full;
        memcpy(out->name, nbuf, EFS_MAX_NAME);
        out->parent = p;
    }
}

int efs_export_lookup(struct efs_export *ex, efs_ino_t parent,
                      const char *name, struct efs_inode *out)
{
    if (!ex || !name)
        return EFS_ERR_INVAL;

    if (export_is_sharded_root(ex)) {
        uint32_t bits = ex->root.shard_bits;
        uint32_t psh = efs_export_shard_of(parent, bits);
        /* Dentry shards only. Walking every extra tab found the child-row
         * name (create_with_ino writes parent+name on ctab), so rename and
         * unlink left exists(old) true after the parent dentry was gone. */
        struct efs_export *ptab = (psh == 0) ? ex
            : efs_export_shard_tab(ex, psh);
        if (efs_export_dir_is_spread(ex, parent)) {
            uint32_t dsh = efs_export_dentry_shard_of(parent, name, bits);
            if (dsh != psh) {
                struct efs_export *htab = (dsh == 0) ? ex
                    : efs_export_shard_tab(ex, dsh);
                if (htab && lookup_on_tab(htab, parent, name, out) == EFS_OK) {
                    if (out)
                        lookup_stitch_child(ex, out);
                    return EFS_OK;
                }
            }
        }
        if (!ptab)
            return EFS_ERR_NOT_FOUND;
        int rc = lookup_on_tab(ptab, parent, name, out);
        if (rc == EFS_OK && out)
            lookup_stitch_child(ex, out);
        return rc;
    }

    uint64_t pos = 0;
    if (name_idx_get(ex, parent, name, &pos) == 0) {
        if (out)
            *out = ex->inodes[pos];
        if (out)
            lookup_stitch_child(ex, out);
        return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
}

int efs_export_get_inode(struct efs_export *ex, efs_ino_t ino,
                         struct efs_inode *out)
{
    if (!ex)
        return EFS_ERR_INVAL;
    if (export_is_sharded_root(ex)) {
        uint32_t sh = efs_export_shard_of(ino, ex->root.shard_bits);
        if (sh != 0) {
            struct efs_export *tab = efs_export_shard_tab(ex, sh);
            if (!tab)
                return EFS_ERR_NOT_FOUND;
            return efs_export_get_inode(tab, ino, out);
        }
    }
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

    efs_export_ensure_rollups(ex);

    /* This insert is table-local. efs_export_lookup on a sharded root
     * walks every loaded extra tab, so the child-row we just wrote on
     * ctab makes the parent-dentry create_with_ino look like EEXIST —
     * readdir of the parent then misses every file whose inode shard
     * is instantiated on this node (1/8 of creates on bits=3). */
    if (lookup_on_tab(ex, parent, name, NULL) == EFS_OK) {
        fprintf(stderr, "cwi-fail: name_dup parent=%llu name=%s ino=%llu "
                "shard=%u cnt=%llu\n", (unsigned long long)parent, name,
                (unsigned long long)ino_num, ex->shard_id,
                (unsigned long long)ex->inode_count);
        return 0;
    }

    if (inode_ptr(ex, ino_num)) {
        fprintf(stderr, "cwi-fail: ino_dup parent=%llu name=%s ino=%llu "
                "shard=%u cnt=%llu\n", (unsigned long long)parent, name,
                (unsigned long long)ino_num, ex->shard_id,
                (unsigned long long)ex->inode_count);
        return 0; /* ino already in use on this table */
    }

    if (ex->inode_count >= ex->inode_capacity) {
        uint64_t new_cap = ex->inode_capacity * 2;
        struct efs_inode *new = realloc(ex->inodes, new_cap * sizeof(struct efs_inode));
        if (!new) {
            fprintf(stderr, "cwi-fail: realloc parent=%llu name=%s ino=%llu\n",
                    (unsigned long long)parent, name,
                    (unsigned long long)ino_num);
            return 0;
        }
        ex->inodes = new;
        ex->inode_capacity = new_cap;
    }
    if (export_ensure_inode_idx(ex) != 0) {
        fprintf(stderr, "cwi-fail: idx parent=%llu name=%s ino=%llu\n",
                (unsigned long long)parent, name,
                (unsigned long long)ino_num);
        return 0;
    }

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
    dentry_bytes_add(ex, ino->name);

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
    parent_touch(ex, parent);
    /* Unsharded create (bits=0) used to leave shard_dirty clear: the flush
     * thread saw rpc_dirty, called flush, then skipped shard 0 — RAM-only
     * mutations and the 70k-ops/s "bench" that never hit disk. */
    ex->shard_dirty = 1;
    efs_export_flush_mark_ino_slot(ex, pos);
    efs_export_flush_mark_dentry_tail(ex);
    return ino->ino;
}

efs_ino_t efs_export_create(struct efs_export *ex, efs_ino_t parent,
                              uint32_t mode, uid_t uid, gid_t gid, const char *name)
{
    if (!ex)
        return 0;
    if (export_is_sharded_root(ex))
        return create_sharded(ex, parent, mode, uid, gid, name);
    efs_ino_t ino = efs_export_alloc_ino(ex, parent);
    if (!ino)
        return 0;
    efs_ino_t created = efs_export_create_with_ino(ex, ino, parent, mode, uid, gid, name);
    if (!created && ex->root.shard_bits == 0 && ex->next_ino == ino + 1)
        ex->next_ino = ino; /* reuse on collision (name exists) */
    return created;
}

/* Touch already-materialized extra tables only. Instantiating every shard
 * here (the old unlink/nlink loops) made the parent owner hold the world.
 * skip_shard is the canonical child table (owner applies nlink/chunks). */
static void for_each_loaded_tab(struct efs_export *ex, uint32_t skip_shard,
                                void (*fn)(struct efs_export *tab, efs_ino_t ino,
                                           uint32_t nlink),
                                efs_ino_t ino, uint32_t nlink)
{
    if (ex->shard_id != skip_shard)
        fn(ex, ino, nlink);
    if (!ex->shard_tabs)
        return;
    for (uint32_t i = 1; i < ex->shard_tab_cap; i++) {
        if (ex->shard_tabs[i] && i != skip_shard)
            fn(ex->shard_tabs[i], ino, nlink);
    }
}

static void tab_unlink_ino(struct efs_export *tab, efs_ino_t ino, uint32_t nlink)
{
    (void)nlink;
    if (efs_export_unlink(tab, ino) == EFS_OK)
        tab->shard_dirty = 1;
}

static void tab_set_nlink(struct efs_export *tab, efs_ino_t ino, uint32_t nlink)
{
    /* Same authoritative-index early-out as efs_export_unlink: this is fanned
     * across every loaded shard table, so the scan must not run on tables that
     * cannot hold the ino. The scan still runs when the ino IS present, since
     * hardlinks put several rows on one table and the index holds only one. */
    if (tab->ino_keys && !inode_ptr(tab, ino))
        return;
    int hit = 0;
    for (uint64_t i = 0; i < tab->inode_count; i++) {
        if (tab->inodes[i].ino == ino) {
            tab->inodes[i].nlink = nlink;
            hit = 1;
        }
    }
    if (hit) {
        tab->shard_dirty = 1;
        for (uint64_t i = 0; i < tab->inode_count; i++) {
            if (tab->inodes[i].ino == ino)
                efs_export_flush_mark_ino_slot(tab, i);
        }
    }
}

int efs_export_unlink_name(struct efs_export *ex, efs_ino_t parent, const char *name)
{
    return efs_export_unlink_name_ex(ex, parent, name, 0);
}

int efs_export_unlink_name_ex(struct efs_export *ex, efs_ino_t parent,
                              const char *name, int keep_last)
{
    if (!ex || !name)
        return EFS_ERR_INVAL;
    if (export_is_sharded_root(ex)) {
        uint32_t bits = ex->root.shard_bits;
        uint32_t psh = efs_export_shard_of(parent, bits);
        struct efs_export *ptab = efs_export_table(ex, psh);
        if (!ptab)
            return EFS_ERR_NOT_FOUND;

        /* Spread: hash-shard dentry first, parent-shard leftover second.
         * Crash between dest-create and source-delete may leave both;
         * hash-shard row wins, parent-shard is the duplicate we drop. */
        struct efs_export *dtab = ptab;
        uint64_t pos = 0;
        if (efs_export_dir_is_spread(ex, parent)) {
            uint32_t dsh = efs_export_dentry_shard_of(parent, name, bits);
            struct efs_export *htab = (dsh == 0) ? ex
                : efs_export_shard_tab(ex, dsh);
            if (htab && name_idx_get(htab, parent, name, &pos) == 0)
                dtab = htab;
        }
        if (dtab == ptab && name_idx_get(ptab, parent, name, &pos) != 0)
            return EFS_ERR_NOT_FOUND;
        if (dtab != ptab && name_idx_get(dtab, parent, name, &pos) != 0)
            return EFS_ERR_NOT_FOUND;
        struct efs_inode removed = dtab->inodes[pos];
        efs_ino_t ino = removed.ino;

        if (efs_mode_is_dir(removed.mode)) {
            if (efs_export_unlink(dtab, ino) == EFS_OK)
                dtab->shard_dirty = 1;
            if (dtab != ptab)
                rollup_sub_under(ptab, parent, &removed);
            parent_touch(ptab, parent);
            return EFS_OK;
        }

        uint32_t nlink = removed.nlink;
        if (nlink == 0)
            nlink = 1;
        nlink--;
        rollup_sub_under(ptab, parent, &removed);
        child_idx_del(dtab, parent, pos);
        parent_touch(ptab, parent);
        remove_inode_slot(dtab, pos, nlink > 0);
        dtab->shard_dirty = 1;
        if (ptab != dtab)
            ptab->shard_dirty = 1;

        /* Parent dentry only. Child nlink / last-link drop is the handler's
         * UNLINK_SHARD (or local nlink_dec) — walking every loaded tab here
         * requires lock_all. */
        return EFS_OK;
    }
    if (strcmp(name, EFS_STATS_NAME) == 0)
        return EFS_ERR_INVAL;
    efs_export_ensure_rollups(ex);

    uint64_t pos = 0;
    if (name_idx_get(ex, parent, name, &pos) != 0)
        return EFS_ERR_NOT_FOUND;

    struct efs_inode removed = ex->inodes[pos];
    efs_ino_t ino = removed.ino;
    uint32_t nlink = removed.nlink;
    if (nlink == 0)
        nlink = 1;
    nlink--;

    /* POSIX: a subdirectory's ".." holds a link on the parent. */
    if (efs_mode_is_dir(removed.mode)) {
        struct efs_inode *p = inode_ptr(ex, parent);
        if (p && p->nlink > 2)
            p->nlink--;
    }

    rollup_sub_under(ex, parent, &removed);
    child_idx_del(ex, parent, pos);
    parent_touch(ex, parent);
    if (nlink == 0 && keep_last) {
        /* Drop the directory name but keep the inode + chunks so an
         * already-open fd on another client can still read. */
        name_idx_del(ex, parent, removed.name);
        dentry_bytes_sub(ex, removed.name);
        ex->inodes[pos].nlink = 0;
        ex->inodes[pos].name[0] = '\0';
        ex->inodes[pos].parent = 0;
        ex->shard_dirty = 1;
        return EFS_OK;
    }

    remove_inode_slot(ex, pos, nlink > 0);
    ex->shard_dirty = 1;

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

    /* The ino index is authoritative (see inode_ptr): a miss means this table
     * holds no row for the ino, so the scan below cannot find one. Skip it.
     * unlink/rename fan this call across EVERY loaded shard table
     * (for_each_loaded_tab), so without the early-out a single unlink costs
     * O(total inodes in the export) — the same full-scan-under-a-fan shape
     * that made stamp_ctime_loaded 9% of efsd on a grown table. */
    if (ex->ino_keys && !inode_ptr(ex, ino))
        return EFS_ERR_NOT_FOUND;

    /* Remove every directory name for this inode, then chunks. */
    int found = 0;
    uint64_t i = 0;
    while (i < ex->inode_count) {
        if (ex->inodes[i].ino == ino) {
            found = 1;
            struct efs_inode removed = ex->inodes[i];
            if (efs_mode_is_dir(removed.mode)) {
                struct efs_inode *p = inode_ptr(ex, removed.parent);
                if (p && p->nlink > 2)
                    p->nlink--;
            }
            rollup_sub_under(ex, removed.parent, &removed);
            child_idx_del(ex, removed.parent, i);
            remove_inode_slot(ex, i, 0);
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

static void shard_set_nlink(struct efs_export *ex, efs_ino_t src_ino,
                            uint32_t nlink)
{
    for_each_loaded_tab(ex, UINT32_MAX, tab_set_nlink, src_ino, nlink);
}

int efs_export_nlink_inc(struct efs_export *ex, efs_ino_t src_ino,
                         struct efs_inode *out)
{
    if (!ex || !src_ino)
        return EFS_ERR_INVAL;
    struct efs_export *ctab = efs_export_table_for_ino(ex, src_ino);
    struct efs_inode *csrc = ctab ? inode_ptr(ctab, src_ino) : NULL;
    if (!csrc)
        return EFS_ERR_NOT_FOUND;
    if (efs_mode_is_dir(csrc->mode))
        return EFS_ERR_INVAL;
    inode_bump_ctime(csrc);
    uint64_t ct = csrc->ctime;
    uint32_t nlink = csrc->nlink + 1;
    shard_set_nlink(ex, src_ino, nlink);
    stamp_ctime_loaded(ex, src_ino, ct);
    ctab->shard_dirty = 1;
    if (out) {
        csrc = inode_ptr(ctab, src_ino);
        if (csrc)
            *out = *csrc;
    }
    return EFS_OK;
}

int efs_export_nlink_dec(struct efs_export *ex, efs_ino_t src_ino,
                         struct efs_inode *out)
{
    return efs_export_nlink_dec_ex(ex, src_ino, out, 0);
}

int efs_export_nlink_dec_ex(struct efs_export *ex, efs_ino_t src_ino,
                            struct efs_inode *out, int keep_last)
{
    if (!ex || !src_ino)
        return EFS_ERR_INVAL;
    struct efs_export *ctab = efs_export_table_for_ino(ex, src_ino);
    struct efs_inode *csrc = ctab ? inode_ptr(ctab, src_ino) : NULL;
    if (!csrc)
        return EFS_ERR_NOT_FOUND;
    uint32_t nlink = csrc->nlink;
    if (nlink == 0)
        nlink = 1;
    nlink--;
    if (nlink == 0) {
        if (keep_last) {
            shard_set_nlink(ex, src_ino, 0);
            csrc = inode_ptr(ctab, src_ino);
            if (csrc) {
                csrc->name[0] = '\0';
                csrc->parent = 0;
            }
            ctab->shard_dirty = 1;
            if (out && csrc)
                *out = *csrc;
            return EFS_OK;
        }
        /* Canonical child table only — lock_all is gone from UNLINK. */
        if (efs_export_unlink(ctab, src_ino) == EFS_OK)
            ctab->shard_dirty = 1;
        if (out)
            memset(out, 0, sizeof(*out));
        return EFS_OK;
    }
    shard_set_nlink(ex, src_ino, nlink);
    ctab->shard_dirty = 1;
    if (out) {
        csrc = inode_ptr(ctab, src_ino);
        if (csrc)
            *out = *csrc;
    }
    return EFS_OK;
}

int efs_export_purge_unlinked(struct efs_export *ex, efs_ino_t ino)
{
    if (!ex || !ino)
        return EFS_ERR_INVAL;
    struct efs_inode cur;
    if (efs_export_get_inode(ex, ino, &cur) != 0)
        return EFS_ERR_NOT_FOUND;
    if (cur.nlink != 0)
        return EFS_OK;
    return efs_export_unlink(ex, ino);
}

int efs_export_link_dentry(struct efs_export *ex, const struct efs_inode *src,
                           efs_ino_t new_parent, const char *new_name)
{
    if (!ex || !src || !src->ino || !new_name || !*new_name)
        return EFS_ERR_INVAL;
    if (strcmp(new_name, EFS_STATS_NAME) == 0)
        return EFS_ERR_INVAL;
    if (efs_mode_is_dir(src->mode))
        return EFS_ERR_INVAL;
    if (efs_export_lookup(ex, new_parent, new_name, NULL) == EFS_OK)
        return EFS_ERR_EXIST;

    uint32_t bits = ex->root.shard_bits;
    struct efs_export *ptab =
        export_is_sharded_root(ex)
            ? efs_export_table(ex, efs_export_shard_of(new_parent, bits))
            : ex;
    struct efs_inode *pdir = ptab ? inode_ptr(ptab, new_parent) : NULL;
    if (!pdir || !efs_mode_is_dir(pdir->mode))
        return EFS_ERR_INVAL;

    efs_export_ensure_rollups(ptab);
    if (ptab->inode_count >= ptab->inode_capacity) {
        uint64_t new_cap = ptab->inode_capacity * 2;
        struct efs_inode *nw =
            realloc(ptab->inodes, new_cap * sizeof(struct efs_inode));
        if (!nw)
            return EFS_ERR_NOMEM;
        ptab->inodes = nw;
        ptab->inode_capacity = new_cap;
    }
    if (export_ensure_inode_idx(ptab) != 0)
        return EFS_ERR_NOMEM;

    shard_set_nlink(ex, src->ino, src->nlink);

    uint64_t pos = ptab->inode_count++;
    struct efs_inode *dst = &ptab->inodes[pos];
    *dst = *src;
    dst->parent = new_parent;
    dst->nlink = src->nlink;
    inode_clear_rollups(dst);
    strncpy(dst->name, new_name, EFS_MAX_NAME - 1);
    dst->name[EFS_MAX_NAME - 1] = '\0';
    dentry_bytes_add(ptab, dst->name);
    time_now(&dst->ctime);
    name_idx_put(ptab, new_parent, new_name, pos);
    child_idx_add(ptab, new_parent, pos);
    rollup_add_under(ptab, new_parent, dst);
    parent_touch(ptab, new_parent);
    ptab->shard_dirty = 1;
    return EFS_OK;
}

int efs_export_link(struct efs_export *ex, efs_ino_t src_ino,
                    efs_ino_t new_parent, const char *new_name)
{
    if (!ex || !new_name || !*new_name)
        return EFS_ERR_INVAL;
    if (strcmp(new_name, EFS_STATS_NAME) == 0)
        return EFS_ERR_INVAL;

    /* Sharded root: the canonical src row lives on the CHILD shard's table
     * and the new dentry belongs on the NEW PARENT's shard table. The
     * unsharded path below would look src up only in the main (shard-0)
     * table — NOT_FOUND for any other shard, so the link never landed
     * server-side (posix hardlink_terminal_ln on bits>0). When the child
     * row is on a peer, the caller (LINK handler) nlink_inc's remotely
     * and uses efs_export_link_dentry. */
    if (export_is_sharded_root(ex)) {
        if (efs_export_lookup(ex, new_parent, new_name, NULL) == EFS_OK)
            return EFS_ERR_EXIST;
        struct efs_inode bumped;
        memset(&bumped, 0, sizeof(bumped));
        int rc = efs_export_nlink_inc(ex, src_ino, &bumped);
        if (rc != EFS_OK)
            return rc;
        return efs_export_link_dentry(ex, &bumped, new_parent, new_name);
    }

    efs_export_ensure_rollups(ex);

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

    inode_bump_ctime(src);
    uint64_t ct = src->ctime;
    uint32_t nlink = src->nlink + 1;
    sync_hardlink_attrs(ex, src_ino, src);
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        if (ex->inodes[i].ino == src_ino)
            ex->inodes[i].nlink = nlink;
    }
    stamp_ctime_loaded(ex, src_ino, ct);
    src = inode_ptr(ex, src_ino);

    uint64_t pos = ex->inode_count++;
    struct efs_inode *dst = &ex->inodes[pos];
    *dst = *src;
    dst->parent = new_parent;
    dst->nlink = nlink;
    inode_clear_rollups(dst);
    strncpy(dst->name, new_name, EFS_MAX_NAME - 1);
    dst->name[EFS_MAX_NAME - 1] = '\0';
    dentry_bytes_add(ex, dst->name);
    time_now(&dst->ctime);

    /* Keep ino_idx pointing at an existing row; name index gets the new name. */
    name_idx_put(ex, new_parent, new_name, pos);
    child_idx_add(ex, new_parent, pos);
    rollup_add_under(ex, new_parent, dst);
    parent_touch(ex, new_parent);
    ex->shard_dirty = 1;
    efs_export_flush_mark_ino_slot(ex, pos);
    efs_export_flush_mark_dentry_tail(ex);
    return EFS_OK;
}

/* Attr accessors want the canonical row, which on a sharded export lives on
 * the child shard's table (same routing as efs_export_get_inode). The
 * parent-shard dentry copy carries name/parent only. */
static struct efs_export *shard_route(struct efs_export *ex, efs_ino_t ino)
{
    if (export_is_sharded_root(ex)) {
        struct efs_export *tab = efs_export_table_for_ino(ex, ino);
        if (tab)
            return tab;
    }
    return ex;
}

static int set_size_common(struct efs_export *ex, efs_ino_t ino, uint64_t size,
                           int do_rollups)
{
    if (!ex)
        return EFS_ERR_INVAL;
    ex = shard_route(ex, ino);
    if (do_rollups)
        efs_export_ensure_rollups(ex);
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
    flush_mark_ptr(ex, p);
    if (!do_rollups) {
        int64_t delta = 0;
        if (!efs_mode_is_dir(p->mode) && size != old_size)
            delta = (int64_t)size - (int64_t)old_size;
        if (pending_rollup_note(ex, ino, delta, 1) != 0)
            return EFS_ERR_NOMEM;
        ex->rollups_stale = 1;
        return EFS_OK;
    }
    if (!efs_mode_is_dir(p->mode) && size != old_size) {
        int64_t delta = (int64_t)size - (int64_t)old_size;
        apply_size_delta_for_ino(ex, ino, delta);
    }
    rollup_expand_parents_of(ex, ino); /* set_size stamps mtime=now: expand */
    return EFS_OK;
}

int efs_export_set_size(struct efs_export *ex, efs_ino_t ino, uint64_t size)
{
    return set_size_common(ex, ino, size, 1);
}

int efs_export_set_size_norollup(struct efs_export *ex, efs_ino_t ino,
                                 uint64_t size)
{
    return set_size_common(ex, ino, size, 0);
}

int efs_export_set_mode(struct efs_export *ex, efs_ino_t ino, uint32_t mode)
{
    if (!ex)
        return EFS_ERR_INVAL;
    ex = shard_route(ex, ino);
    efs_export_ensure_rollups(ex);
    struct efs_inode *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    p->mode = (p->mode & S_IFMT) | (mode & ~S_IFMT);
    /* POSIX: chmod updates ctime, not mtime (rsync -a relies on this).
     * ctime is stored at second resolution; if chmod lands in the same
     * second, still advance so st_ctime_ns is observably newer. */
    {
        uint64_t old = p->ctime;
        time_now(&p->ctime);
        if (p->ctime <= old)
            p->ctime = old + 1;
    }
    sync_hardlink_attrs(ex, ino, p);
    flush_mark_ptr(ex, p);
    rollup_expand_parents_of(ex, ino); /* ctime=now: expand */
    return EFS_OK;
}

int efs_export_set_owner(struct efs_export *ex, efs_ino_t ino, uid_t uid, gid_t gid)
{
    if (!ex)
        return EFS_ERR_INVAL;
    ex = shard_route(ex, ino);
    efs_export_ensure_rollups(ex);
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
    flush_mark_ptr(ex, p);
    rollup_expand_parents_of(ex, ino); /* ctime=now: expand */
    return EFS_OK;
}

static int set_mtime_ns_common(struct efs_export *ex, efs_ino_t ino,
                               uint64_t mtime, uint32_t mtime_nsec,
                               int do_rollups)
{
    if (!ex)
        return EFS_ERR_INVAL;
    ex = shard_route(ex, ino);
    if (do_rollups)
        efs_export_ensure_rollups(ex);
    struct efs_inode *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    if (mtime_nsec >= 1000000000u)
        mtime_nsec = 0;
    p->mtime = mtime;
    p->mtime_nsec = mtime_nsec;
    sync_hardlink_attrs(ex, ino, p);
    flush_mark_ptr(ex, p);
    if (!do_rollups) {
        if (pending_rollup_note(ex, ino, 0, 1) != 0)
            return EFS_ERR_NOMEM;
        ex->rollups_stale = 1;
        return EFS_OK;
    }
    /* entry_tmax is dominated by ctime (=now, never back-dated), so even an
     * explicit utimens only widens the parent range — expand incrementally. */
    rollup_expand_parents_of(ex, ino);
    return EFS_OK;
}

int efs_export_set_mtime_ns(struct efs_export *ex, efs_ino_t ino,
                            uint64_t mtime, uint32_t mtime_nsec)
{
    return set_mtime_ns_common(ex, ino, mtime, mtime_nsec, 1);
}

int efs_export_set_mtime_ns_norollup(struct efs_export *ex, efs_ino_t ino,
                                     uint64_t mtime, uint32_t mtime_nsec)
{
    return set_mtime_ns_common(ex, ino, mtime, mtime_nsec, 0);
}

int efs_export_set_mtime(struct efs_export *ex, efs_ino_t ino, uint64_t mtime)
{
    return efs_export_set_mtime_ns(ex, ino, mtime, 0);
}

int efs_export_set_atime(struct efs_export *ex, efs_ino_t ino, uint64_t atime)
{
    if (!ex)
        return EFS_ERR_INVAL;
    ex = shard_route(ex, ino);
    efs_export_ensure_rollups(ex);
    struct efs_inode *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    p->atime = atime;
    sync_hardlink_attrs(ex, ino, p);
    flush_mark_ptr(ex, p);
    rollup_expand_parents_of(ex, ino);
    return EFS_OK;
}

int efs_export_rename_at(struct efs_export *ex, efs_ino_t old_parent,
                         const char *old_name, efs_ino_t new_parent,
                         const char *new_name)
{
    if (!ex || !old_name || !new_name)
        return EFS_ERR_INVAL;
    struct efs_export *tab = ex;
    if (export_is_sharded_root(ex) && ex->root.shard_bits) {
        uint32_t sh = efs_export_shard_of(old_parent, ex->root.shard_bits);
        if (sh != 0) {
            tab = efs_export_table(ex, sh);
            if (!tab)
                return EFS_ERR_NOT_FOUND;
        }
    }
    uint64_t pos = 0;
    if (name_idx_get(tab, old_parent, old_name, &pos) != 0)
        return EFS_ERR_NOT_FOUND;
    efs_ino_t ino = tab->inodes[pos].ino;
    if (tab->ino_keys)
        idx_put(tab->ino_keys, tab->ino_vals, tab->ino_mask, ino, pos);
    return efs_export_rename(tab == ex ? ex : tab, ino, new_parent, new_name);
}

int efs_export_rename(struct efs_export *ex, efs_ino_t ino,
                      efs_ino_t new_parent, const char *new_name)
{
    if (!ex || !new_name)
        return EFS_ERR_INVAL;
    if (strcmp(new_name, EFS_STATS_NAME) == 0)
        return EFS_ERR_INVAL;
    efs_export_ensure_rollups(ex);

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
        /* Same-dir rename: slot stays in the parent child-vec. */
        if (old_parent != new_parent)
            child_idx_del(ex, old_parent, slot);
        name_idx_del(ex, src->parent, src->name);
        dentry_bytes_sub(ex, src->name);
        strncpy(src->name, new_name, EFS_MAX_NAME - 1);
        src->name[EFS_MAX_NAME - 1] = '\0';
        dentry_bytes_add(ex, src->name);
        src->parent = new_parent;
        /* Preserve mtime across rename (rsync partial → final); bump ctime
         * on every hard-link row so lstat of the other name sees it. */
        inode_bump_ctime(src);
        stamp_ctime_loaded(ex, ino, src->ctime);
        name_idx_put(ex, src->parent, src->name, slot);
        if (old_parent != new_parent)
            child_idx_add(ex, new_parent, slot);
        if (old_parent != new_parent) {
            if (efs_mode_is_dir(src->mode)) {
                struct efs_inode *op = inode_ptr(ex, old_parent);
                struct efs_inode *np = inode_ptr(ex, new_parent);
                if (op && op->nlink > 2)
                    op->nlink--;
                if (np)
                    np->nlink++;
            }
            rollup_add_under(ex, new_parent, src);
            parent_touch(ex, old_parent);
            parent_touch(ex, new_parent);
        } else {
            /* Same-dir rename only bumps ctime (time-forward). Expand
             * in O(depth) — recompute_times_up rescans every child and
             * made rsync temp→final O(n²) in a wide directory. */
            expand_parent_chain(ex, src->ino, new_parent,
                                entry_tmin(src), entry_tmax(src));
            parent_touch(ex, new_parent);
        }
    }
    /* Same-count rename only rewrites the dentry tail and the compact slot.
     * mark_full would disable incremental serialize for the whole ecopy
     * temp→final storm (every file is a rename). Overwrite already mark_full
     * via unlink → remove_inode_slot. */
    efs_export_flush_mark_ino_slot(ex, slot);
    efs_export_flush_mark_dentry_all(ex);
    return EFS_OK;
}

int efs_export_set_chunk(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         const efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS],
                         const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    if (!ex)
        return EFS_ERR_INVAL;
    if (export_is_sharded_root(ex)) {
        struct efs_export *tab = efs_export_table_for_chunk(ex, ino,
                                                           chunk_index);
        if (tab && tab != ex)
            return efs_export_set_chunk(tab, ino, chunk_index, fragment_nodes,
                                        checksums);
    }

    uint64_t pos = 0;
    if (chunk_idx_get(ex, ino, chunk_index, &pos) == 0) {
        memcpy(ex->chunks[pos].fragment_nodes, fragment_nodes,
               sizeof(efs_node_id_t) * EFS_NUM_FRAGMENTS);
        memcpy(ex->chunks[pos].checksums, checksums,
               EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
        ex->chunk_epoch++;
        efs_export_flush_mark_chunk_slot(ex, pos);
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
    icnt_inc(ex, ino);
    ex->chunk_epoch++;
    efs_export_flush_mark_chunk_slot(ex, pos);
    return EFS_OK;
}

int efs_export_get_chunk(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         struct efs_chunk_entry *out)
{
    if (!ex)
        return EFS_ERR_INVAL;
    if (export_is_sharded_root(ex)) {
        struct efs_export *tab = efs_export_table_for_chunk(ex, ino,
                                                           chunk_index);
        if (tab && tab != ex)
            return efs_export_get_chunk(tab, ino, chunk_index, out);
    }

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
            dentry_bytes_add(ex, ci->name);
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
            dentry_bytes_add(ex, ci->name);
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
            icnt_inc(ex, cc->ino);
        }
        ex->chunk_epoch++;
    }

    if (inc->next_ino > ex->next_ino)
        ex->next_ino = inc->next_ino;

    /* Rollups are derived; rebuild after merge so newer-wins cannot leave
     * stale aggregates relative to the combined child set. */
    efs_export_recompute_rollups(ex);
    return EFS_OK;
}

int efs_export_upsert_inode(struct efs_export *ex, const struct efs_inode *rec)
{
    if (!ex || !rec || rec->ino == 0)
        return EFS_ERR_INVAL;
    if (export_is_sharded_root(ex)) {
        struct efs_export *tab = efs_export_table_for_ino(ex, rec->ino);
        if (tab && tab != ex)
            return efs_export_upsert_inode(tab, rec);
    }
    if (!ex->ino_keys && export_reindex(ex) != 0)
        return EFS_ERR_NOMEM;

    struct efs_inode *cur = inode_ptr(ex, rec->ino);
    if (!cur) {
        if (grow_inodes(ex) != 0)
            return EFS_ERR_NOMEM;
        if (export_ensure_inode_idx(ex) != 0)
            return EFS_ERR_NOMEM;
        uint64_t pos = ex->inode_count++;
        ex->inodes[pos] = *rec;
        dentry_bytes_add(ex, rec->name);
        idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, rec->ino, pos);
        name_idx_put(ex, rec->parent, rec->name, pos);
        child_idx_add(ex, rec->parent, pos);
        efs_export_flush_mark_ino_slot(ex, pos);
        efs_export_flush_mark_dentry_tail(ex);
        return EFS_OK;
    }
    if (cur->parent != rec->parent || strcmp(cur->name, rec->name) != 0) {
        /* Rebind the existing row (rename between base fetch and rebase):
         * same index dance as efs_export_rename. */
        uint64_t slot = (uint64_t)(cur - ex->inodes);
        child_idx_del(ex, cur->parent, slot);
        name_idx_del(ex, cur->parent, cur->name);
        dentry_bytes_sub(ex, cur->name);
        *cur = *rec;
        dentry_bytes_add(ex, rec->name);
        name_idx_put(ex, rec->parent, rec->name, slot);
        child_idx_add(ex, rec->parent, slot);
        sync_hardlink_attrs(ex, rec->ino, cur);
        efs_export_flush_mark_full(ex);
        return EFS_OK;
    }
    *cur = *rec;
    /* Hard-link rows share mode/uid/times; upsert of the indexed row
     * alone would leave the other names with a stale mode. */
    sync_hardlink_attrs(ex, rec->ino, cur);
    flush_mark_ptr(ex, cur);
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

void efs_export_pack_header_ver(const struct efs_export *ex,
                                uint8_t out[EFS_META_HDR_SIZE], uint32_t ver)
{
    uint8_t *p = out;
#define W_RAW(v, n) do { memcpy(p, (v), (n)); p += (n); } while (0)
#define W_32(v) do { uint32_t v_ = (uint32_t)(v); memcpy(p, &v_, 4); p += 4; } while (0)
#define W_64(v) do { uint64_t v_ = (uint64_t)(v); memcpy(p, &v_, 8); p += 8; } while (0)
    W_RAW(EFS_META_MAGIC, 4);
    W_32(ver);
    W_32(ex->id);
    {
        char name[EFS_MAX_NAME];
        memset(name, 0, sizeof(name));
        strncpy(name, ex->name, EFS_MAX_NAME - 1);
        W_RAW(name, EFS_MAX_NAME);
    }
    W_64(ex->next_ino);
    W_32((uint32_t)ex->inode_count);
    W_32((uint32_t)ex->chunk_count);
#undef W_RAW
#undef W_32
#undef W_64
    (void)p;
}

void efs_export_pack_header(const struct efs_export *ex, uint8_t out[EFS_META_HDR_SIZE])
{
    efs_export_pack_header_ver(ex, out, EFS_META_VERSION);
}

void efs_export_pack_inode(const struct efs_inode *ino, uint8_t out[EFS_INODE_WIRE_SIZE])
{
    uint8_t *p = out;
#define W_RAW(v, n) do { memcpy(p, (v), (n)); p += (n); } while (0)
#define W_32(v) do { uint32_t v_ = (uint32_t)(v); memcpy(p, &v_, 4); p += 4; } while (0)
#define W_64(v) do { uint64_t v_ = (uint64_t)(v); memcpy(p, &v_, 8); p += 8; } while (0)
    W_64(ino->ino);
    W_64(ino->parent);
    W_32(ino->mode);
    W_32((uint32_t)ino->uid);
    W_32((uint32_t)ino->gid);
    W_64(ino->size);
    W_64(ino->mtime);
    W_32(ino->mtime_nsec);
    W_64(ino->ctime);
    W_64(ino->atime);
    W_32(ino->nlink);
    W_RAW(ino->name, EFS_MAX_NAME);
    W_64(ino->imm_files);
    W_64(ino->imm_dirs);
    W_64(ino->tree_files);
    W_64(ino->tree_dirs);
    W_64(ino->imm_bytes);
    W_64(ino->tree_bytes);
    W_64(ino->imm_tmin);
    W_64(ino->imm_tmax);
    W_64(ino->tree_tmin);
    W_64(ino->tree_tmax);
    W_64(ino->pack_ino);
    W_32(ino->pack_off);
    W_32(ino->pack_len);
#undef W_RAW
#undef W_32
#undef W_64
    (void)p;
}

void efs_export_pack_inode_compact(const struct efs_inode *ino,
                                   uint8_t out[EFS_INODE_COMPACT_SIZE])
{
    uint8_t *p = out;
#define W_32(v) do { uint32_t v_ = (uint32_t)(v); memcpy(p, &v_, 4); p += 4; } while (0)
#define W_64(v) do { uint64_t v_ = (uint64_t)(v); memcpy(p, &v_, 8); p += 8; } while (0)
    W_64(ino->ino);
    W_64(ino->parent);
    W_32(ino->mode);
    W_32((uint32_t)ino->uid);
    W_32((uint32_t)ino->gid);
    W_64(ino->size);
    W_64(ino->mtime);
    W_32(ino->mtime_nsec);
    W_64(ino->ctime);
    W_64(ino->atime);
    W_32(ino->nlink);
    W_64(ino->imm_files);
    W_64(ino->imm_dirs);
    W_64(ino->imm_bytes);
    W_64(ino->imm_tmin);
    W_64(ino->imm_tmax);
    W_64(ino->pack_ino);
    W_32(ino->pack_off);
    W_32(ino->pack_len);
#undef W_32
#undef W_64
    (void)p;
}

void efs_export_pack_chunk(const struct efs_chunk_entry *ce,
                           uint8_t out[EFS_CHUNK_WIRE_SIZE])
{
    uint8_t *p = out;
    uint64_t ino = ce->ino;
    uint32_t ci = ce->chunk_index;
    memcpy(p, &ino, 8); p += 8;
    memcpy(p, &ci, 4); p += 4;
    memcpy(p, ce->fragment_nodes, sizeof(efs_node_id_t) * EFS_NUM_FRAGMENTS);
    p += sizeof(efs_node_id_t) * EFS_NUM_FRAGMENTS;
    memcpy(p, ce->checksums, EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
}

int efs_export_inode_slot(struct efs_export *ex, efs_ino_t ino, uint64_t *slot)
{
    if (!ex || !slot || ino == 0)
        return EFS_ERR_INVAL;
    uint64_t pos = 0;
    if (ex->ino_keys &&
        idx_get(ex->ino_keys, ex->ino_vals, ex->ino_mask, ino, &pos) == 0 &&
        pos < ex->inode_count && ex->inodes[pos].ino == ino) {
        *slot = pos;
        return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
}

int efs_export_chunk_slot(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                          uint64_t *slot)
{
    if (!ex || !slot)
        return EFS_ERR_INVAL;
    uint64_t pos = 0;
    if (chunk_idx_get(ex, ino, chunk_index, &pos) == 0) {
        *slot = pos;
        return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
}

int efs_export_needs_inode_grow(const struct efs_export *ex)
{
    if (!ex)
        return 1;
    if (ex->inode_count >= ex->inode_capacity)
        return 1;
    if (!ex->ino_keys || ex->inode_count * 2 > ex->ino_mask + 1)
        return 1;
    if (ex->child_vecs && ex->child_vec_count >= ex->child_vec_cap)
        return 1;
    return 0;
}

int efs_export_needs_chunk_grow(const struct efs_export *ex)
{
    if (!ex)
        return 1;
    if (ex->chunk_count >= ex->chunk_capacity)
        return 1;
    if (!ex->chunk_keys || ex->chunk_count * 2 > ex->chunk_mask + 1)
        return 1;
    return 0;
}

int efs_export_reserve_inodes(struct efs_export *ex, uint64_t extra)
{
    if (!ex)
        return EFS_ERR_INVAL;
    uint64_t need = ex->inode_count + (extra ? extra : 1);
    while (ex->inode_capacity < need) {
        if (grow_inodes(ex) != EFS_OK)
            return EFS_ERR_NOMEM;
        /* grow_inodes no-ops when count < cap; force a double. */
        if (ex->inode_capacity < need) {
            uint64_t new_cap = ex->inode_capacity ? ex->inode_capacity * 2 : 16;
            if (new_cap < need)
                new_cap = need;
            struct efs_inode *n = realloc(ex->inodes, new_cap * sizeof(*n));
            if (!n)
                return EFS_ERR_NOMEM;
            ex->inodes = n;
            ex->inode_capacity = new_cap;
        }
    }
    if (export_ensure_inode_idx(ex) != 0)
        return EFS_ERR_NOMEM;
    /* needs_inode_grow also trips when the parent→children table is full.
     * Growing only the inode array left create() spinning and FUSE mapping
     * the failure to EEXIST (ImageNet second-tree copy died at ~123 files). */
    if (export_reserve_child_vecs(ex, extra) != EFS_OK)
        return EFS_ERR_NOMEM;
    return EFS_OK;
}

int efs_export_reserve_chunks(struct efs_export *ex, uint64_t extra)
{
    if (!ex)
        return EFS_ERR_INVAL;
    uint64_t need = ex->chunk_count + (extra ? extra : 1);
    while (ex->chunk_capacity < need) {
        uint64_t new_cap = ex->chunk_capacity ? ex->chunk_capacity * 2 : 16;
        if (new_cap < need)
            new_cap = need;
        struct efs_chunk_entry *n = realloc(ex->chunks, new_cap * sizeof(*n));
        if (!n)
            return EFS_ERR_NOMEM;
        ex->chunks = n;
        ex->chunk_capacity = new_cap;
    }
    if (export_ensure_chunk_idx(ex) != 0)
        return EFS_ERR_NOMEM;
    return EFS_OK;
}

int efs_export_table_snapshot_ex(const struct efs_export *ex,
                                 struct efs_export *snap, int omit_chunks)
{
    if (!ex || !snap)
        return EFS_ERR_INVAL;
    memset(snap, 0, sizeof(*snap));
    snap->id = ex->id;
    memcpy(snap->name, ex->name, EFS_MAX_NAME);
    snap->chunk_size = ex->chunk_size;
    snap->features = ex->features;
    snap->next_ino = ex->next_ino;
    snap->inode_count = ex->inode_count;
    snap->chunk_count = ex->chunk_count;
    snap->dentry_bytes = ex->dentry_bytes;
    if (ex->inode_count) {
        size_t n = (size_t)ex->inode_count * sizeof(*snap->inodes);
        snap->inodes = malloc(n);
        if (!snap->inodes)
            return EFS_ERR_NOMEM;
        memcpy(snap->inodes, ex->inodes, n);
        snap->inode_capacity = ex->inode_count;
    }
    if (ex->chunk_count && !omit_chunks) {
        size_t n = (size_t)ex->chunk_count * sizeof(*snap->chunks);
        snap->chunks = malloc(n);
        if (!snap->chunks) {
            free(snap->inodes);
            snap->inodes = NULL;
            return EFS_ERR_NOMEM;
        }
        memcpy(snap->chunks, ex->chunks, n);
        snap->chunk_capacity = ex->chunk_count;
    }
    return EFS_OK;
}

int efs_export_table_snapshot(const struct efs_export *ex,
                              struct efs_export *snap)
{
    return efs_export_table_snapshot_ex(ex, snap, 0);
}

void efs_export_table_snapshot_free(struct efs_export *snap)
{
    if (!snap)
        return;
    free(snap->inodes);
    free(snap->chunks);
    memset(snap, 0, sizeof(*snap));
}

void efs_export_adopt_tables(struct efs_export *dst, struct efs_export *src)
{
    if (!dst || !src)
        return;
    free(dst->inodes);
    free(dst->chunks);
    free(dst->pending_rollup_inos);
    free(dst->pending_rollup_deltas);
    free(dst->pending_rollup_touch);
    idx_free(&dst->ino_keys, &dst->ino_vals, &dst->ino_mask);
    idx_free(&dst->name_keys, &dst->name_vals, &dst->name_mask);
    idx_free(&dst->chunk_keys, &dst->chunk_vals, &dst->chunk_mask);
    icnt_free(dst);
    child_vecs_free(dst);

    dst->inodes = src->inodes;
    dst->inode_count = src->inode_count;
    dst->inode_capacity = src->inode_capacity;
    dst->dentry_bytes = src->dentry_bytes;
    dst->chunks = src->chunks;
    dst->chunk_count = src->chunk_count;
    dst->chunk_capacity = src->chunk_capacity;
    dst->ino_keys = src->ino_keys;
    dst->ino_vals = src->ino_vals;
    dst->ino_mask = src->ino_mask;
    dst->name_keys = src->name_keys;
    dst->name_vals = src->name_vals;
    dst->name_mask = src->name_mask;
    dst->chunk_keys = src->chunk_keys;
    dst->chunk_vals = src->chunk_vals;
    dst->chunk_mask = src->chunk_mask;
    dst->icnt_keys = src->icnt_keys;
    dst->icnt_vals = src->icnt_vals;
    dst->icnt_mask = src->icnt_mask;
    dst->child_keys = src->child_keys;
    dst->child_vals = src->child_vals;
    dst->child_mask = src->child_mask;
    dst->child_vecs = src->child_vecs;
    dst->child_vec_count = src->child_vec_count;
    dst->child_vec_cap = src->child_vec_cap;
    dst->pending_rollup_inos = src->pending_rollup_inos;
    dst->pending_rollup_deltas = src->pending_rollup_deltas;
    dst->pending_rollup_touch = src->pending_rollup_touch;
    dst->pending_rollup_count = src->pending_rollup_count;
    dst->pending_rollup_cap = src->pending_rollup_cap;
    dst->rollups_stale = src->rollups_stale;
    dst->next_ino = src->next_ino;
    dst->layout_epoch++;
    free(dst->flush_blob);
    dst->flush_blob = NULL;
    dst->flush_blob_gen = 0;
    efs_export_flush_mark_full(dst);

    src->inodes = NULL;
    src->chunks = NULL;
    src->pending_rollup_inos = NULL;
    src->pending_rollup_deltas = NULL;
    src->pending_rollup_touch = NULL;
    src->ino_keys = src->ino_vals = NULL;
    src->name_keys = src->name_vals = NULL;
    src->chunk_keys = src->chunk_vals = NULL;
    src->icnt_keys = NULL;
    src->icnt_vals = NULL;
    src->child_keys = src->child_vals = NULL;
    src->child_vecs = NULL;
    src->inode_count = src->inode_capacity = 0;
    src->chunk_count = src->chunk_capacity = 0;
    src->ino_mask = src->name_mask = src->chunk_mask = src->child_mask = 0;
    src->icnt_mask = 0;
    src->child_vec_count = src->child_vec_cap = 0;
    src->pending_rollup_count = src->pending_rollup_cap = 0;
}

#define EFS_FLUSH_HDR_ICOUNT_OFF 276

int efs_export_serialize_dirty(struct efs_export *ex,
                               const char *cache, uint32_t cache_ino_len,
                               uint32_t cache_chunk_len, int omit_chunks,
                               char **buf, size_t *len,
                               uint32_t *ino_blob_len, uint32_t *chunk_blob_len,
                               int *used_incr)
{
    if (used_incr)
        *used_incr = 0;
    if (!ex || !buf || !len)
        return EFS_ERR_INVAL;
    efs_export_ensure_rollups(ex);
    dentry_bytes_recompute(ex);

    if (!cache || ex->flush_full || cache_ino_len < EFS_META_HDR_SIZE)
        return EFS_OK;

    uint32_t ver = 0, old_ic = 0;
    memcpy(&ver, cache + 4, 4);
    memcpy(&old_ic, cache + EFS_FLUSH_HDR_ICOUNT_OFF, 4);
    if (ver != EFS_META_VERSION || ex->inode_count < old_ic)
        return EFS_OK;

    size_t dent_off = efs_meta_dent_off(EFS_META_VERSION, ex->inode_count);
    size_t old_dent_off = efs_meta_dent_off(EFS_META_VERSION, old_ic);
    if (dent_off != old_dent_off || cache_ino_len < old_dent_off)
        return EFS_OK;

    size_t dent_bytes = dentry_bytes_for(ex, 0);
    size_t new_ino = dent_off + dent_bytes;
    size_t old_dent_bytes = (size_t)cache_ino_len - old_dent_off;
    if (efs_meta_page_count_for_blob((uint32_t)new_ino) !=
        efs_meta_page_count_for_blob(cache_ino_len))
        return EFS_OK;
    if (new_ino > (size_t)UINT32_MAX)
        return EFS_ERR_INVAL;

    size_t chunk_bytes = omit_chunks ? 0
        : (size_t)ex->chunk_count * EFS_CHUNK_WIRE_SIZE;
    size_t total = new_ino + chunk_bytes;
    uint8_t *b = calloc(1, total ? total : 1);
    if (!b)
        return EFS_ERR_NOMEM;

    size_t copy_ino = cache_ino_len < (uint32_t)new_ino ? cache_ino_len
                                                       : (size_t)new_ino;
    memcpy(b, cache, copy_ino);
    efs_export_pack_header(ex, b);

    uint32_t ino_pc = efs_meta_page_count_for_blob((uint32_t)new_ino);
    flush_mark_page(ex, 0);
    for (uint64_t s = old_ic; s < ex->inode_count; s++)
        efs_export_flush_mark_ino_slot(ex, s);
    for (uint32_t pi = 0; pi < ino_pc; pi++) {
        uint64_t start = (uint64_t)pi * EFS_META_PAGE_SIZE;
        uint64_t end = start + EFS_META_PAGE_SIZE;
        if (end > cache_ino_len && start < new_ino)
            flush_mark_page(ex, pi);
        if (start >= dent_off)
            continue;
        if (!efs_export_flush_page_is_dirty(ex, pi))
            continue;
        if (end <= EFS_META_HDR_SIZE)
            continue;
        uint64_t s0 = start > EFS_META_HDR_SIZE
            ? (start - EFS_META_HDR_SIZE) / EFS_INODE_COMPACT_SIZE : 0;
        uint64_t s1 = (end - EFS_META_HDR_SIZE - 1) / EFS_INODE_COMPACT_SIZE + 1;
        if (s1 > ex->inode_count)
            s1 = ex->inode_count;
        for (uint64_t slot = s0; slot < s1; slot++) {
            uint64_t off = (uint64_t)EFS_META_HDR_SIZE +
                           slot * (uint64_t)EFS_INODE_COMPACT_SIZE;
            if (off >= dent_off)
                break;
            efs_export_pack_inode_compact(&ex->inodes[slot], b + off);
        }
    }

    int rewrite_dents = (dent_bytes != old_dent_bytes);
    if (!rewrite_dents) {
        uint32_t d0 = (uint32_t)(dent_off / EFS_META_PAGE_SIZE);
        for (uint32_t pi = d0; pi < ino_pc; pi++) {
            if (efs_export_flush_page_is_dirty(ex, pi)) {
                rewrite_dents = 1;
                break;
            }
        }
    }
    if (rewrite_dents) {
        uint8_t *p = b + dent_off;
        uint8_t *end = b + new_ino;
        for (uint64_t slot = 0; slot < ex->inode_count; slot++) {
            uint16_t ln = (uint16_t)strnlen(ex->inodes[slot].name,
                                            EFS_MAX_NAME - 1);
            if (p + 2u + (size_t)ln > end) {
                free(b);
                return EFS_OK;
            }
            memcpy(p, &ln, 2);
            p += 2;
            if (ln) {
                memcpy(p, ex->inodes[slot].name, ln);
                p += ln;
            }
        }
        if (p != end) {
            free(b);
            return EFS_OK;
        }
        efs_export_flush_mark_dentry_all(ex);
    } else if (ex->inode_count > old_ic) {
        uint8_t *p = b + cache_ino_len;
        uint8_t *end = b + new_ino;
        for (uint64_t slot = old_ic; slot < ex->inode_count; slot++) {
            uint16_t ln = (uint16_t)strnlen(ex->inodes[slot].name,
                                            EFS_MAX_NAME - 1);
            if (p + 2u + (size_t)ln > end) {
                free(b);
                return EFS_OK;
            }
            memcpy(p, &ln, 2);
            p += 2;
            if (ln) {
                memcpy(p, ex->inodes[slot].name, ln);
                p += ln;
            }
        }
        if (p != end) {
            free(b);
            return EFS_OK;
        }
    }

    if (!omit_chunks) {
        size_t new_ch = (size_t)ex->chunk_count * EFS_CHUNK_WIRE_SIZE;
        size_t copy_ch = cache_chunk_len;
        if (copy_ch > new_ch)
            copy_ch = new_ch;
        if (copy_ch)
            memcpy(b + new_ino, cache + cache_ino_len, copy_ch);
        uint32_t ch_pc = efs_meta_page_count_for_blob((uint32_t)new_ch);
        for (uint32_t pi = 0; pi < ch_pc; pi++) {
            uint32_t packed = ino_pc + pi;
            uint64_t start = (uint64_t)pi * EFS_META_PAGE_SIZE;
            uint64_t end = start + EFS_META_PAGE_SIZE;
            int is_new = start >= cache_chunk_len;
            if (!is_new && !efs_export_flush_page_is_dirty(ex, packed))
                continue;
            if (is_new)
                flush_mark_page(ex, packed);
            uint64_t s0 = start / EFS_CHUNK_WIRE_SIZE;
            uint64_t s1 = (end - 1) / EFS_CHUNK_WIRE_SIZE + 1;
            if (s1 > ex->chunk_count)
                s1 = ex->chunk_count;
            for (uint64_t s = s0; s < s1; s++) {
                uint64_t off = s * (uint64_t)EFS_CHUNK_WIRE_SIZE;
                efs_export_pack_chunk(&ex->chunks[s], b + new_ino + off);
            }
        }
    }

    *buf = (char *)b;
    *len = total;
    if (ino_blob_len)
        *ino_blob_len = (uint32_t)new_ino;
    if (chunk_blob_len)
        *chunk_blob_len = omit_chunks ? cache_chunk_len
                                      : (uint32_t)chunk_bytes;
    if (used_incr)
        *used_incr = 1;
    return EFS_OK;
}

int efs_export_serialize_ex(struct efs_export *ex, char **buf, size_t *len,
                            uint32_t *ino_blob_len, uint32_t *chunk_blob_len)
{
    if (!ex || !buf || !len)
        return EFS_ERR_INVAL;
    efs_export_ensure_rollups(ex);

    /* v7: compact inode rows, zero pad to the page-aligned dentry offset,
     * packed dentries, then the chunk region. The pad keeps the dentry region
     * stable as inode_count grows within a compact page (see
     * efs_meta_dent_off).
     * Size from the names we will write — a stale-low dentry_bytes made
     * malloc short and the dentry loop smashed the next heap chunk
     * (SIGABRT "corrupted size vs. prev_size" in snapshot_free). */
    dentry_bytes_recompute(ex);
    size_t dent_bytes = dentry_bytes_for(ex, 0);
    size_t chunk_bytes = (size_t)ex->chunk_count * EFS_CHUNK_WIRE_SIZE;
    size_t dent_off = efs_meta_dent_off(EFS_META_VERSION, ex->inode_count);
    size_t total = dent_off + dent_bytes + chunk_bytes;

    uint8_t *b = malloc(total ? total : 1);
    if (!b)
        return EFS_ERR_NOMEM;
    uint8_t *p = b;

#define NEED(n) do { \
        if ((size_t)(p - b) + (size_t)(n) > total) { \
            free(b); \
            return EFS_ERR_INVAL; \
        } \
    } while (0)
#define W_RAW(v, n) do { NEED(n); memcpy(p, (v), (n)); p += (n); } while (0)
#define W_32(v) do { uint32_t v_ = (uint32_t)(v); W_RAW(&v_, 4); } while (0)
#define W_64(v) do { uint64_t v_ = (uint64_t)(v); W_RAW(&v_, 8); } while (0)

    W_RAW(EFS_META_MAGIC, 4);
    W_32(EFS_META_VERSION);
    W_32(ex->id);
    {
        char name[EFS_MAX_NAME];
        memset(name, 0, sizeof(name));
        strncpy(name, ex->name, EFS_MAX_NAME - 1);
        W_RAW(name, EFS_MAX_NAME);
    }
    W_64(ex->next_ino);
    W_32((uint32_t)ex->inode_count);
    W_32((uint32_t)ex->chunk_count);

    for (uint64_t i = 0; i < ex->inode_count; i++) {
        NEED(EFS_INODE_COMPACT_SIZE);
        efs_export_pack_inode_compact(&ex->inodes[i], p);
        p += EFS_INODE_COMPACT_SIZE;
    }
    /* Zero pad to the page-aligned dentry offset (v7). */
    if ((size_t)(p - b) < dent_off) {
        memset(p, 0, dent_off - (size_t)(p - b));
        p = b + dent_off;
    }
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        uint16_t ln = (uint16_t)strnlen(ex->inodes[i].name, EFS_MAX_NAME - 1);
        NEED(2u + (size_t)ln);
        memcpy(p, &ln, 2);
        p += 2;
        if (ln) {
            memcpy(p, ex->inodes[i].name, ln);
            p += ln;
        }
    }
    uint32_t ino_len = (uint32_t)(p - b);

    if (ex->chunks) {
        for (uint64_t i = 0; i < ex->chunk_count; i++) {
            struct efs_chunk_entry *ce = &ex->chunks[i];
            W_64(ce->ino);
            W_32(ce->chunk_index);
            W_RAW(ce->fragment_nodes, sizeof(efs_node_id_t) * EFS_NUM_FRAGMENTS);
            W_RAW(ce->checksums, EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
        }
    }
#undef NEED
#undef W_RAW
#undef W_32
#undef W_64

    *buf = (char *)b;
    *len = (size_t)(p - b);
    if (ino_blob_len)
        *ino_blob_len = ino_len;
    if (chunk_blob_len)
        *chunk_blob_len = (uint32_t)(*len - ino_len);
    return EFS_OK;
}

int efs_export_serialize(struct efs_export *ex, char **buf, size_t *len)
{
    return efs_export_serialize_ex(ex, buf, len, NULL, NULL);
}

int efs_export_deserialize(struct efs_export *ex, const char *buf, size_t len)
{
    if (!ex || !buf)
        return EFS_ERR_INVAL;

    /* Table swap: any cached GET_META serialize of the old table is stale. */
    free(ex->gm_blob);
    ex->gm_blob = NULL;

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
        (version != 2 && version != 3 && version != 4 && version != 5 &&
         version != 6 && version != 7)) {
        fclose(f);
        return EFS_ERR_PROTO;
    }

    efs_export_free(ex);
    efs_export_init(ex, 0, "");
    ex->meta_fragmented = 0;

    read_u32(f, &ex->id);
    if (version >= 5) {
        if (fread(ex->name, EFS_MAX_NAME, 1, f) != 1) {
            fclose(f);
            return EFS_ERR_PROTO;
        }
        ex->name[EFS_MAX_NAME - 1] = '\0';
    } else {
        read_str(f, ex->name, EFS_MAX_NAME);
    }
    read_u64(f, &ex->next_ino);

    uint32_t inode_count;
    uint32_t v5_chunk_count = 0;
    if (read_u32(f, &inode_count) != 0) {
        fclose(f);
        return EFS_ERR_PROTO;
    }
    if (version >= 5 && read_u32(f, &v5_chunk_count) != 0) {
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
        if (version >= 6) {
            read_u64(f, &ino->imm_files);
            read_u64(f, &ino->imm_dirs);
            read_u64(f, &ino->imm_bytes);
            read_u64(f, &ino->imm_tmin);
            read_u64(f, &ino->imm_tmax);
            read_u64(f, &ino->pack_ino);
            read_u32(f, &ino->pack_off);
            read_u32(f, &ino->pack_len);
        } else if (version >= 5) {
            if (fread(ino->name, EFS_MAX_NAME, 1, f) != 1) {
                fclose(f);
                return EFS_ERR_PROTO;
            }
            ino->name[EFS_MAX_NAME - 1] = '\0';
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
            read_u64(f, &ino->pack_ino);
            read_u32(f, &ino->pack_off);
            read_u32(f, &ino->pack_len);
        } else {
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
    }

    if (version >= 6) {
        /* v7 page-aligns the dentry region within the inode region; skip the
         * zero pad between the compact rows and the first dentry. */
        if (version >= 7) {
            long dent_off = (long)efs_meta_dent_off(version, inode_count);
            if (dent_off > 0 && dent_off <= (long)len)
                fseek(f, dent_off, SEEK_SET);
        }
        /* Dentry region bounds: chunks occupy the fixed-size tail, so the
         * dentry region ends chunk_count*WIRE bytes before the blob end. A
         * holed (zero-filled) meta page inside the dentry area desyncs the
         * walk — garbage lengths used to fail the whole deserialize (PROTO),
         * which fenced server tables into a rebuild livelock. Bound the walk
         * and hole out unreadable names instead; the rows stay addressable
         * under a synthetic name until a later client flush re-publishes the
         * real page content. */
        long dent_end = (long)len -
                        (long)v5_chunk_count * (long)EFS_CHUNK_WIRE_SIZE;
        int holed = 0;
        for (uint32_t i = 0; i < inode_count && i < ex->inode_count; i++) {
            long pos = ftell(f);
            uint16_t ln = 0;
            memset(ex->inodes[i].name, 0, EFS_MAX_NAME);
            if (!holed && pos >= 0 && pos + 2 <= dent_end &&
                fread(&ln, 2, 1, f) == 1 && ln < EFS_MAX_NAME &&
                pos + 2 + ln <= dent_end &&
                (!ln || fread(ex->inodes[i].name, ln, 1, f) == 1)) {
                /* A torn page can also yield a name the walk consumes
                 * cleanly but the kernel rejects in readdir — empty, or
                 * containing '/' / control bytes — turning ls into EIO.
                 * The stream is still in sync, so just rename the row. */
                uint64_t rino = ex->inodes[i].ino;
                if (rino != 0 && rino != EFS_ROOT_INO) {
                    int bad = (ln == 0);
                    for (uint16_t k = 0; !bad && k < ln; k++)
                        if (ex->inodes[i].name[k] == '/' ||
                            (unsigned char)ex->inodes[i].name[k] < 0x20)
                            bad = 1;
                    if (bad)
                        snprintf(ex->inodes[i].name, EFS_MAX_NAME,
                                 ".efshole.%llu", (unsigned long long)rino);
                }
                continue;
            }
            snprintf(ex->inodes[i].name, EFS_MAX_NAME, ".efshole.%llu",
                     (unsigned long long)ex->inodes[i].ino);
            holed = 1; /* stream position untrustworthy from here on */
        }
        if (holed) {
            fprintf(stderr,
                    "meta: deserialize tolerated holed dentry region; "
                    "some names synthetic\n");
            if (dent_end > 0)
                fseek(f, dent_end, SEEK_SET);
        }
    }

    uint32_t chunk_count;
    if (version >= 5) {
        chunk_count = v5_chunk_count;
    } else if (read_u32(f, &chunk_count) != 0) {
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
        struct efs_chunk_entry *ce = &ex->chunks[ex->chunk_count];
        if (read_u64(f, &ce->ino) != 0 ||
            read_u32(f, &ce->chunk_index) != 0 ||
            fread(ce->fragment_nodes, sizeof(efs_node_id_t),
                  EFS_NUM_FRAGMENTS, f) != EFS_NUM_FRAGMENTS ||
            fread(ce->checksums, EFS_HASH_SIZE, EFS_NUM_FRAGMENTS, f) !=
                EFS_NUM_FRAGMENTS)
            break; /* truncated tail (holed page): keep what decoded */
        /* A zero-filled meta page in the chunk region decodes as all-zero
         * records; ino 0 is never valid, so drop the hole rows. */
        if (ce->ino == 0)
            continue;
        ex->chunk_count++;
    }

    fclose(f);
    ex->efsm_version = version >= 7 ? EFS_META_EFSM_V7
                       : version >= 6 ? EFS_META_EFSM_V6
                                      : EFS_META_EFSM_V5;
    export_drop_zero_inodes(ex);
    dentry_bytes_recompute(ex);
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
    return (blob_len + EFS_META_PAGE_SIZE - 1) / EFS_META_PAGE_SIZE;
}

static uint32_t page_count_u64(uint64_t blob_len)
{
    if (blob_len == 0)
        return 0;
    if (blob_len > (uint64_t)UINT32_MAX)
        return UINT32_MAX;
    return efs_meta_page_count_for_blob((uint32_t)blob_len);
}

static size_t dentry_rec_len(const char *name)
{
    return 2 + strnlen(name ? name : "", EFS_MAX_NAME - 1);
}

static void dentry_bytes_add(struct efs_export *ex, const char *name)
{
    if (ex)
        ex->dentry_bytes += dentry_rec_len(name);
}

static void dentry_bytes_sub(struct efs_export *ex, const char *name)
{
    if (!ex)
        return;
    size_t n = dentry_rec_len(name);
    if (ex->dentry_bytes >= n)
        ex->dentry_bytes -= n;
    else
        ex->dentry_bytes = 0;
}

static void dentry_bytes_recompute(struct efs_export *ex)
{
    if (!ex)
        return;
    uint64_t n = 0;
    for (uint64_t i = 0; i < ex->inode_count; i++)
        n += dentry_rec_len(ex->inodes[i].name);
    ex->dentry_bytes = n;
}

static size_t dentry_bytes_for(const struct efs_export *ex, uint64_t extra_inodes)
{
    size_t n = ex ? (size_t)ex->dentry_bytes : 0;
    /* New rows: budget a max-length name so create cannot sneak past the cap. */
    n += (size_t)extra_inodes * (2 + (EFS_MAX_NAME - 1));
    return n;
}

int efs_export_fits_page_cap(const struct efs_export *ex, uint64_t extra_inodes,
                             uint64_t extra_chunks)
{
    if (!ex)
        return 0;
    uint64_t cc = ex->chunk_count + extra_chunks;
    if (page_count_u64(cc * (uint64_t)EFS_CHUNK_WIRE_SIZE) > EFS_META_CHUNK_PAGE_MAX)
        return 0;
    /* Adding chunks does not grow the inode region — skip name accounting. */
    if (extra_inodes == 0)
        return 1;
    uint64_t ic = ex->inode_count + extra_inodes;
    uint64_t ino_blob = (uint64_t)efs_meta_dent_off(EFS_META_VERSION, ic) +
                        dentry_bytes_for(ex, extra_inodes);
    return page_count_u64(ino_blob) <= EFS_META_INO_PAGE_MAX;
}

void efs_export_meta_page_usage(const struct efs_export *ex,
                                uint32_t *ino_pages, uint32_t *chunk_pages)
{
    uint32_t ip = 0, cp = 0;
    if (ex) {
        uint64_t ino_blob = (uint64_t)efs_meta_dent_off(EFS_META_VERSION,
                                                        ex->inode_count) +
                            dentry_bytes_for(ex, 0);
        ip = page_count_u64(ino_blob);
        cp = page_count_u64(ex->chunk_count * (uint64_t)EFS_CHUNK_WIRE_SIZE);
    }
    if (ino_pages)
        *ino_pages = ip;
    if (chunk_pages)
        *chunk_pages = cp;
}

uint32_t efs_export_shard_of(efs_ino_t ino, uint32_t shard_bits)
{
    if (shard_bits == 0)
        return 0;
    /* The root inode and its dentries always live on the main table. */
    if (ino == EFS_ROOT_INO)
        return 0;
    /* Low bits select the shard: shard s owns the congruence class
     * ino == s (mod 2^bits), so every shard has an unbounded ino space.
     * (The old ino >> bits scheme gave each shard only 2^bits inos, so
     * shard 0 exhausted after two creates and every mkdir failed with
     * EEXIST; shard ids were also unbounded, overflowing shard_tabs.) */
    return (uint32_t)((uint64_t)ino & ((1ull << shard_bits) - 1));
}

uint32_t efs_export_chunk_shard_of(efs_ino_t ino, uint32_t chunk_index,
                                   uint32_t shard_bits)
{
    if (shard_bits == 0)
        return 0;
    uint32_t base = efs_export_shard_of(ino, shard_bits);
    uint32_t group = chunk_index >> EFS_CHUNK_GROUP_SHIFT;
    uint32_t mask = (1u << shard_bits) - 1u;
    /* Group 0 stays on the inode shard so a small file's mappings and
     * inode row share an owner (one GETCHUNKS hop). Later groups mix. */
    uint32_t mix = group ? (uint32_t)hash_mix((uint64_t)group) : 0;
    return base ^ (mix & mask);
}

struct efs_export *efs_export_table_for_chunk(struct efs_export *ex,
                                              efs_ino_t ino,
                                              uint32_t chunk_index)
{
    if (!ex)
        return NULL;
    if (!export_is_sharded_root(ex))
        return ex;
    return efs_export_table(ex, efs_export_chunk_shard_of(ino, chunk_index,
                                                         ex->root.shard_bits));
}

uint32_t efs_export_dentry_shard_of(efs_ino_t parent, const char *name,
                                    uint32_t shard_bits)
{
    if (shard_bits == 0 || !name)
        return 0;
    uint64_t h = hash_name_key(parent, name);
    return (uint32_t)(h & ((1ull << shard_bits) - 1));
}

int efs_inode_dir_is_spread(const struct efs_inode *dir)
{
    if (!dir)
        return 0;
    return (dir->imm_files + dir->imm_dirs) >= EFS_DIR_SPREAD_MIN;
}

int efs_export_dir_is_spread(struct efs_export *ex, efs_ino_t dir)
{
    struct efs_inode d;
    if (!ex || !dir)
        return 0;
    if (efs_export_get_inode(ex, dir, &d) != 0)
        return 0;
    return efs_inode_dir_is_spread(&d);
}

efs_node_id_t efs_shard_owner_of(uint32_t shard, uint32_t shard_count,
                                 const efs_node_id_t *live, uint32_t nlive)
{
    if (!live || nlive == 0)
        return 0;
    /* Canonical order: sort ascending. Callers build their live lists in
     * different orders (client: cluster order; server: self first), and
     * live[shard % nlive] on unsorted lists made every node compute a
     * different owner for the same shard — non-shard-0 recs were dropped
     * by everyone and no node flushed the extra shards. */
    efs_node_id_t sorted[EFS_MAX_NODES];
    uint32_t n = 0;
    for (uint32_t i = 0; i < nlive && n < EFS_MAX_NODES; i++) {
        if (live[i] == 0)
            continue;
        uint32_t j = n++;
        while (j > 0 && sorted[j - 1] > live[i]) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = live[i];
    }
    if (n == 0)
        return 0;
    if (shard_count <= 1)
        return sorted[0]; /* lowest live id = metadata primary */
    return sorted[shard % n];
}

efs_ino_t efs_export_alloc_ino(struct efs_export *ex, efs_ino_t parent)
{
    if (!ex)
        return 0;
    uint32_t bits = ex->root.shard_bits;
    if (bits == 0) {
        efs_ino_t ino = ex->next_ino;
        if (ino <= EFS_ROOT_INO)
            ino = EFS_ROOT_INO + 1;
        ex->next_ino = ino + 1;
        return ino;
    }
    /* Low-bits sharding: this table owns the congruence class
     * ino == shard_id (mod 2^bits); probe that class. With 2^bits shards
     * each class is unbounded, so a shard can never "fill up" the way the
     * old [shard << bits, (shard+1) << bits) window did after 2^bits
     * creates. The parent argument is unused here: create_sharded picks
     * the target table, and a table allocates only from its own class. */
    (void)parent;
    uint32_t shard = ex->shard_id;
    uint64_t span = 1ull << bits;
    const uint64_t limit = 1ull << 40; /* sanity bound, ~2^32 goal is 2^32 */
    uint64_t cand = ex->next_ino;
    if (cand >= limit || (cand & (span - 1)) != shard)
        cand = shard;
    while (cand <= EFS_ROOT_INO)
        cand += span; /* ino 0 is invalid; ino 1 is the root */
    while (cand < limit) {
        if (efs_export_get_inode(ex, (efs_ino_t)cand, NULL) != 0) {
            ex->next_ino = (efs_ino_t)(cand + span);
            return (efs_ino_t)cand;
        }
        cand += span;
    }
    return 0;
}

efs_ino_t efs_export_alloc_ino_for_shard(struct efs_export *ex, uint32_t shard)
{
    if (!ex)
        return 0;
    struct efs_export *ctab = efs_export_table(ex, shard);
    if (!ctab)
        return 0;
    /* Shard-local next_ino is authoritative for this congruence class.
     * Probing the main table's ino index here was the per-op CREATE
     * ino_dup: that index lives on shard 0 and was read without its lock. */
    return efs_export_alloc_ino(ctab, shard);
}

int efs_meta_extract_page(const char *blob, uint32_t blob_len, uint32_t page_index,
                          uint8_t page_out[EFS_META_PAGE_SIZE])
{
    if (!blob || !page_out)
        return EFS_ERR_INVAL;
    uint32_t pages = efs_meta_page_count_for_blob(blob_len);
    if (page_index >= pages)
        return EFS_ERR_INVAL;
    memset(page_out, 0, EFS_META_PAGE_SIZE);
    uint32_t off = page_index * EFS_META_PAGE_SIZE;
    uint32_t n = blob_len - off;
    if (n > EFS_META_PAGE_SIZE)
        n = EFS_META_PAGE_SIZE;
    memcpy(page_out, blob + off, n);
    return EFS_OK;
}

int efs_meta_assemble_blob(const struct efs_export_root *root,
                           const uint8_t pages[][EFS_META_PAGE_SIZE],
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
    if (root->chunk_page_count > 0) {
        uint32_t ino_pc = root->ino_page_count;
        uint32_t ino_len = root->ino_blob_len;
        uint32_t ch_len = root->chunk_blob_len;
        for (uint32_t i = 0; i < ino_pc; i++) {
            uint32_t off = i * EFS_META_PAGE_SIZE;
            uint32_t n = (off < ino_len) ? ino_len - off : 0;
            if (n > EFS_META_PAGE_SIZE)
                n = EFS_META_PAGE_SIZE;
            if (n)
                memcpy(blob + off, pages[i], n);
        }
        for (uint32_t i = 0; i < root->chunk_page_count; i++) {
            uint32_t off = i * EFS_META_PAGE_SIZE;
            uint32_t n = (off < ch_len) ? ch_len - off : 0;
            if (n > EFS_META_PAGE_SIZE)
                n = EFS_META_PAGE_SIZE;
            if (n)
                memcpy(blob + ino_len + off, pages[ino_pc + i], n);
        }
    } else {
        for (uint32_t i = 0; i < root->page_count; i++) {
            uint32_t off = i * EFS_META_PAGE_SIZE;
            uint32_t n = root->blob_len - off;
            if (n > EFS_META_PAGE_SIZE)
                n = EFS_META_PAGE_SIZE;
            memcpy(blob + off, pages[i], n);
        }
    }
    *blob_out = blob;
    *blob_len_out = root->blob_len;
    return EFS_OK;
}

void efs_export_root_free(struct efs_export_root *root)
{
    if (!root)
        return;
    if (root->extra_roots) {
        for (uint32_t i = 0; i < root->extra_shard_count; i++)
            efs_export_root_free(&root->extra_roots[i]);
        free(root->extra_roots);
        root->extra_roots = NULL;
    }
    free(root->extra_shard_ids);
    root->extra_shard_ids = NULL;
    root->extra_shard_count = 0;
    free(root->page_checksums);
    root->page_checksums = NULL;
    free(root->page_cis);
    root->page_cis = NULL;
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
    dst->page_cis = NULL;
    dst->extra_shard_ids = NULL;
    dst->extra_roots = NULL;
    dst->extra_shard_count = 0;
    if (src->page_count > 0 && src->page_checksums) {
        size_t n = (size_t)src->page_count * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE;
        dst->page_checksums = malloc(n);
        if (!dst->page_checksums)
            return EFS_ERR_NOMEM;
        memcpy(dst->page_checksums, src->page_checksums, n);
        if (src->page_cis) {
            size_t cn = (size_t)src->page_count * sizeof(uint32_t);
            dst->page_cis = malloc(cn);
            if (!dst->page_cis) {
                free(dst->page_checksums);
                dst->page_checksums = NULL;
                return EFS_ERR_NOMEM;
            }
            memcpy(dst->page_cis, src->page_cis, cn);
        }
    }
    if (src->extra_shard_count && src->extra_roots && src->extra_shard_ids) {
        dst->extra_shard_ids = malloc((size_t)src->extra_shard_count *
                                      sizeof(uint32_t));
        dst->extra_roots = calloc(src->extra_shard_count,
                                  sizeof(struct efs_export_root));
        if (!dst->extra_shard_ids || !dst->extra_roots) {
            efs_export_root_free(dst);
            return EFS_ERR_NOMEM;
        }
        memcpy(dst->extra_shard_ids, src->extra_shard_ids,
               (size_t)src->extra_shard_count * sizeof(uint32_t));
        dst->extra_shard_count = src->extra_shard_count;
        for (uint32_t i = 0; i < src->extra_shard_count; i++) {
            if (efs_export_root_copy(&dst->extra_roots[i],
                                     &src->extra_roots[i]) != EFS_OK) {
                efs_export_root_free(dst);
                return EFS_ERR_NOMEM;
            }
        }
    }
    return EFS_OK;
}

int efs_export_root_prepare(struct efs_export_root *root,
                            const struct efs_export *ex,
                            uint64_t generation,
                            uint32_t ino_blob_len,
                            uint32_t chunk_blob_len)
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
    root->ino_blob_len = ino_blob_len;
    root->chunk_blob_len = chunk_blob_len;
    root->blob_len = ino_blob_len + chunk_blob_len;
    root->ino_page_count = efs_meta_page_count_for_blob(ino_blob_len);
    root->chunk_page_count = efs_meta_page_count_for_blob(chunk_blob_len);
    root->page_count = root->ino_page_count + root->chunk_page_count;
    root->chunk_size = ex->chunk_size ? ex->chunk_size : EFS_DEFAULT_CHUNK_SIZE;
    if (!efs_chunk_size_valid(root->chunk_size))
        root->chunk_size = EFS_DEFAULT_CHUNK_SIZE;
    root->features = ex->features;
    root->shard_count = ex->root.shard_count ? ex->root.shard_count : 1;
    root->shard_bits = ex->root.shard_bits;
    root->write_lease_id = ex->root.write_lease_id;
    root->write_lease_until_ms = ex->root.write_lease_until_ms;
    root->next_ci = ex->root.next_ci;
    if (root->ino_page_count > EFS_META_INO_PAGE_MAX ||
        root->chunk_page_count > EFS_META_CHUNK_PAGE_MAX)
        return EFS_ERR_INVAL;
    if (root->page_count > 0) {
        size_t n = (size_t)root->page_count * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE;
        root->page_checksums = calloc(1, n);
        if (!root->page_checksums)
            return EFS_ERR_NOMEM;
        if (root->version >= EFS_META_ROOT_VERSION_V7) {
            root->page_cis = calloc(root->page_count, sizeof(uint32_t));
            if (!root->page_cis)
                return EFS_ERR_NOMEM;
        }
    }
    return efs_export_root_capture_extras(root, ex);
}

int efs_export_root_capture_extras(struct efs_export_root *root,
                                   const struct efs_export *ex)
{
    if (!root || !ex)
        return EFS_ERR_INVAL;
    if (root->extra_roots) {
        for (uint32_t i = 0; i < root->extra_shard_count; i++)
            efs_export_root_free(&root->extra_roots[i]);
        free(root->extra_roots);
        root->extra_roots = NULL;
    }
    free(root->extra_shard_ids);
    root->extra_shard_ids = NULL;
    root->extra_shard_count = 0;
    if (!ex->shard_tabs || root->version < EFS_META_ROOT_VERSION_V8)
        return EFS_OK;
    uint32_t n = 0;
    for (uint32_t i = 1; i < ex->shard_tab_cap; i++) {
        struct efs_export *tab = ex->shard_tabs[i];
        if (tab && tab->root.page_count && tab->root.page_checksums)
            n++;
    }
    if (!n)
        return EFS_OK;
    root->extra_shard_ids = malloc((size_t)n * sizeof(uint32_t));
    root->extra_roots = calloc(n, sizeof(struct efs_export_root));
    if (!root->extra_shard_ids || !root->extra_roots)
        return EFS_ERR_NOMEM;
    uint32_t w = 0;
    for (uint32_t i = 1; i < ex->shard_tab_cap && w < n; i++) {
        struct efs_export *tab = ex->shard_tabs[i];
        if (!tab || !tab->root.page_count || !tab->root.page_checksums)
            continue;
        root->extra_shard_ids[w] = i;
        if (efs_export_root_copy(&root->extra_roots[w], &tab->root) != EFS_OK)
            return EFS_ERR_NOMEM;
        w++;
    }
    root->extra_shard_count = w;
    return EFS_OK;
}

/* Max-merge prev's extra-shard descriptors into root: per shard id, keep the
 * higher-generation descriptor, and carry forward shards that root lacks
 * entirely. A cluster root is the recovery record for EVERY shard — an
 * adopt/commit that drops or regresses a descriptor orphans that shard's
 * tables (the efs-s3 clobber: a catchup adopt of an extras-less root wiped
 * the descriptors, the rebuild then reinstalled nothing, and the next
 * commit persisted the loss). */
int efs_export_root_maxmerge_extras(struct efs_export_root *root,
                                    const struct efs_export_root *prev)
{
    if (!root || !prev)
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < prev->extra_shard_count; i++) {
        uint32_t sh = prev->extra_shard_ids ? prev->extra_shard_ids[i] : 0;
        if (sh == 0)
            continue;
        const struct efs_export_root *p = &prev->extra_roots[i];
        uint32_t j = 0;
        while (j < root->extra_shard_count && root->extra_shard_ids[j] != sh)
            j++;
        if (j < root->extra_shard_count) {
            if (root->extra_roots[j].generation >= p->generation)
                continue; /* root's descriptor is fresher-or-equal: keep it */
            efs_export_root_free(&root->extra_roots[j]);
            if (efs_export_root_copy(&root->extra_roots[j], p) != EFS_OK)
                return EFS_ERR_NOMEM;
            continue;
        }
        uint32_t n = root->extra_shard_count;
        uint32_t *ids = realloc(root->extra_shard_ids,
                                (size_t)(n + 1) * sizeof(uint32_t));
        struct efs_export_root *er =
            realloc(root->extra_roots,
                    (size_t)(n + 1) * sizeof(struct efs_export_root));
        if (!ids || !er) {
            /* A failed realloc leaves the original arrays owned by root —
             * free only whichever NEW allocation succeeded. */
            if (ids)
                free(ids);
            if (er)
                free(er);
            return EFS_ERR_NOMEM;
        }
        root->extra_shard_ids = ids;
        root->extra_roots = er;
        memset(&root->extra_roots[n], 0, sizeof(root->extra_roots[n]));
        if (efs_export_root_copy(&root->extra_roots[n], p) != EFS_OK)
            return EFS_ERR_NOMEM;
        root->extra_shard_ids[n] = sh;
        root->extra_shard_count = n + 1;
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
    /* Keep the live layout version. Rewriting v4 as v5 made joiners/clients
     * look up pages at the 32k-window offsets while fragments still lived
     * in the v4 8k window. */
    {
        uint32_t ver = root->version;
        if (ver < EFS_META_ROOT_VERSION_V4 || ver > EFS_META_ROOT_VERSION_V8)
            ver = EFS_META_ROOT_VERSION_V8;
        write_u32(f, ver);
    }
    write_u32(f, root->id);
    write_str(f, root->name);
    write_u64(f, root->next_ino);
    write_u64(f, root->generation);
    write_u32(f, root->blob_len);
    write_u32(f, root->page_count);
    {
        uint32_t cs = root->chunk_size ? root->chunk_size : EFS_DEFAULT_CHUNK_SIZE;
        write_u32(f, cs);
    }
    write_u32(f, root->features);
    write_u32(f, root->ino_blob_len);
    write_u32(f, root->chunk_blob_len);
    write_u32(f, root->ino_page_count);
    write_u32(f, root->chunk_page_count);
    if (root->version >= EFS_META_ROOT_VERSION_V6) {
        uint32_t sc = root->shard_count ? root->shard_count : 1;
        write_u32(f, sc);
        write_u32(f, root->shard_bits);
        write_u64(f, root->write_lease_id);
        write_u64(f, root->write_lease_until_ms);
    }
    if (root->version >= EFS_META_ROOT_VERSION_V7)
        write_u32(f, root->next_ci);
    if (root->page_count > 0) {
        size_t n = (size_t)root->page_count * EFS_NUM_FRAGMENTS * EFS_HASH_SIZE;
        fwrite(root->page_checksums, 1, n, f);
        if (root->version >= EFS_META_ROOT_VERSION_V7) {
            if (!root->page_cis) {
                fclose(f);
                return EFS_ERR_INVAL;
            }
            fwrite(root->page_cis, sizeof(uint32_t), root->page_count, f);
        }
    }
    if (root->version >= EFS_META_ROOT_VERSION_V8) {
        write_u32(f, root->extra_shard_count);
        for (uint32_t i = 0; i < root->extra_shard_count; i++) {
            write_u32(f, root->extra_shard_ids ? root->extra_shard_ids[i] : 0);
            char *ibuf = NULL;
            size_t ilen = 0;
            if (!root->extra_roots ||
                efs_export_root_serialize(&root->extra_roots[i], &ibuf,
                                          &ilen) != EFS_OK) {
                free(ibuf);
                fclose(f);
                return EFS_ERR_INVAL;
            }
            write_u32(f, (uint32_t)ilen);
            if (ilen && fwrite(ibuf, 1, ilen, f) != ilen) {
                free(ibuf);
                fclose(f);
                return EFS_ERR_IO;
            }
            free(ibuf);
        }
    }
    fclose(f);
    return EFS_OK;
}

int efs_export_root_deserialize(struct efs_export_root *root,
                                const char *buf, size_t len)
{
    return efs_export_root_deserialize_used(root, buf, len, NULL);
}

int efs_export_root_deserialize_used(struct efs_export_root *root,
                                     const char *buf, size_t len, size_t *used)
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
    if (read_u32(f, &version) != 0 ||
        (version != EFS_META_ROOT_VERSION_V1 &&
         version != EFS_META_ROOT_VERSION_V2 &&
         version != EFS_META_ROOT_VERSION_V3 &&
         version != EFS_META_ROOT_VERSION_V4 &&
         version != EFS_META_ROOT_VERSION_V5 &&
         version != EFS_META_ROOT_VERSION_V6 &&
         version != EFS_META_ROOT_VERSION_V7 &&
         version != EFS_META_ROOT_VERSION_V8)) {
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
    if (version >= EFS_META_ROOT_VERSION_V2) {
        if (read_u32(f, &root->chunk_size) != 0) {
            fclose(f);
            return EFS_ERR_PROTO;
        }
    } else {
        root->chunk_size = EFS_DEFAULT_CHUNK_SIZE;
    }
    if (!efs_chunk_size_valid(root->chunk_size))
        root->chunk_size = EFS_DEFAULT_CHUNK_SIZE;
    if (version >= EFS_META_ROOT_VERSION_V3) {
        if (read_u32(f, &root->features) != 0) {
            fclose(f);
            return EFS_ERR_PROTO;
        }
    } else {
        root->features = EFS_FEATURES_DEFAULT;
    }
    if (version >= EFS_META_ROOT_VERSION_V4) {
        if (read_u32(f, &root->ino_blob_len) != 0 ||
            read_u32(f, &root->chunk_blob_len) != 0 ||
            read_u32(f, &root->ino_page_count) != 0 ||
            read_u32(f, &root->chunk_page_count) != 0) {
            fclose(f);
            return EFS_ERR_PROTO;
        }
    } else {
        root->ino_blob_len = root->blob_len;
        root->chunk_blob_len = 0;
        root->ino_page_count = root->page_count;
        root->chunk_page_count = 0;
    }
    if (version >= EFS_META_ROOT_VERSION_V6) {
        if (read_u32(f, &root->shard_count) != 0 ||
            read_u32(f, &root->shard_bits) != 0 ||
            read_u64(f, &root->write_lease_id) != 0 ||
            read_u64(f, &root->write_lease_until_ms) != 0) {
            fclose(f);
            return EFS_ERR_PROTO;
        }
        if (root->shard_count == 0)
            root->shard_count = 1;
    } else {
        root->shard_count = 1;
        root->shard_bits = 0;
        root->write_lease_id = 0;
        root->write_lease_until_ms = 0;
    }
    if (version >= EFS_META_ROOT_VERSION_V7) {
        if (read_u32(f, &root->next_ci) != 0) {
            fclose(f);
            return EFS_ERR_PROTO;
        }
    } else {
        root->next_ci = 0;
    }
    if (root->page_count > EFS_META_MAX_PAGES) {
        fclose(f);
        return EFS_ERR_PROTO;
    }
    if (version >= EFS_META_ROOT_VERSION_V4) {
        uint32_t ino_lim = (version >= EFS_META_ROOT_VERSION_V5)
                               ? EFS_META_INO_PAGE_MAX
                               : EFS_META_V4_INO_MAX;
        uint32_t ch_lim = (version >= EFS_META_ROOT_VERSION_V5)
                              ? EFS_META_CHUNK_PAGE_MAX
                              : EFS_META_V4_CHUNK_MAX;
        if (root->ino_page_count + root->chunk_page_count != root->page_count ||
            root->ino_blob_len + root->chunk_blob_len != root->blob_len ||
            root->ino_page_count > ino_lim ||
            root->chunk_page_count > ch_lim) {
            fclose(f);
            return EFS_ERR_PROTO;
        }
    } else if (root->page_count != efs_meta_page_count_for_blob(root->blob_len)) {
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
        if (version >= EFS_META_ROOT_VERSION_V7) {
            size_t cn = (size_t)root->page_count * sizeof(uint32_t);
            root->page_cis = malloc(cn);
            if (!root->page_cis) {
                free(root->page_checksums);
                root->page_checksums = NULL;
                fclose(f);
                return EFS_ERR_NOMEM;
            }
            if (fread(root->page_cis, sizeof(uint32_t), root->page_count, f) !=
                root->page_count) {
                free(root->page_cis);
                free(root->page_checksums);
                root->page_cis = NULL;
                root->page_checksums = NULL;
                fclose(f);
                return EFS_ERR_PROTO;
            }
        }
    }
    if (version >= EFS_META_ROOT_VERSION_V8) {
        uint32_t n = 0;
        if (read_u32(f, &n) != 0) {
            fclose(f);
            return EFS_ERR_PROTO;
        }
        if (n > EFS_META_MAX_SHARDS) {
            fclose(f);
            return EFS_ERR_PROTO;
        }
        if (n) {
            root->extra_shard_ids = malloc((size_t)n * sizeof(uint32_t));
            root->extra_roots = calloc(n, sizeof(struct efs_export_root));
            if (!root->extra_shard_ids || !root->extra_roots) {
                fclose(f);
                return EFS_ERR_NOMEM;
            }
            root->extra_shard_count = n;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t sid = 0, ilen = 0;
                if (read_u32(f, &sid) != 0 || read_u32(f, &ilen) != 0 ||
                    ilen == 0 || ilen > 64u * 1024u * 1024u) {
                    fclose(f);
                    return EFS_ERR_PROTO;
                }
                root->extra_shard_ids[i] = sid;
                char *ibuf = malloc(ilen);
                if (!ibuf) {
                    fclose(f);
                    return EFS_ERR_NOMEM;
                }
                if (fread(ibuf, 1, ilen, f) != ilen) {
                    free(ibuf);
                    fclose(f);
                    return EFS_ERR_PROTO;
                }
                int irc = efs_export_root_deserialize(&root->extra_roots[i],
                                                      ibuf, ilen);
                free(ibuf);
                if (irc != EFS_OK) {
                    fclose(f);
                    return irc;
                }
            }
        }
    }
    if (used) {
        long pos = ftell(f);
        *used = (pos > 0) ? (size_t)pos : 0;
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
            ex->chunk_size = efs_chunk_size_valid(root.chunk_size)
                                 ? root.chunk_size
                                 : EFS_DEFAULT_CHUNK_SIZE;
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
