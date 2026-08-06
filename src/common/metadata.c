#include "efs/metadata.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#define EFS_META_MAGIC "EFSM"
#define EFS_META_ROOT_MAGIC "EFSR"
/* v2: uid/gid. v3: mtime_nsec after mtime. */
#define EFS_META_VERSION 3
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
        ex->inodes[i] = *src;
        ex->inodes[i].parent = parent;
        memcpy(ex->inodes[i].name, name, EFS_MAX_NAME);
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

/* Swap-remove inode array slot i; refresh indexes for the moved row. */
static void remove_inode_slot(struct efs_export *ex, uint64_t i)
{
    efs_ino_t old_parent = ex->inodes[i].parent;
    char old_name[EFS_MAX_NAME];
    memcpy(old_name, ex->inodes[i].name, EFS_MAX_NAME);
    efs_ino_t old_ino = ex->inodes[i].ino;

    name_idx_del(ex, old_parent, old_name);

    uint64_t last = ex->inode_count - 1;
    if (i != last) {
        idx_del(ex->ino_keys, ex->ino_vals, ex->ino_mask, ex->inodes[last].ino);
        name_idx_del(ex, ex->inodes[last].parent, ex->inodes[last].name);
        ex->inodes[i] = ex->inodes[last];
        ex->inode_count--;
        idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, ex->inodes[i].ino, i);
        name_idx_put(ex, ex->inodes[i].parent, ex->inodes[i].name, i);
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
    ex->inodes[ex->inode_count++] = root;
    export_reindex(ex);
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
    strncpy(ino->name, name, EFS_MAX_NAME - 1);

    idx_put(ex->ino_keys, ex->ino_vals, ex->ino_mask, ino_num, pos);
    name_idx_put(ex, parent, name, pos);

    if (efs_mode_is_dir(mode)) {
        ino->nlink = 2;
        struct efs_inode *p = inode_ptr(ex, parent);
        if (p)
            p->nlink++;
    }

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

    uint64_t pos = 0;
    if (name_idx_get(ex, parent, name, &pos) != 0)
        return EFS_ERR_NOT_FOUND;

    efs_ino_t ino = ex->inodes[pos].ino;
    uint32_t nlink = ex->inodes[pos].nlink;
    if (nlink == 0)
        nlink = 1;
    nlink--;

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
    strncpy(dst->name, new_name, EFS_MAX_NAME - 1);
    dst->name[EFS_MAX_NAME - 1] = '\0';
    time_now(&dst->ctime);

    /* Keep ino_idx pointing at an existing row; name index gets the new name. */
    name_idx_put(ex, new_parent, new_name, pos);
    return EFS_OK;
}

int efs_export_set_size(struct efs_export *ex, efs_ino_t ino, uint64_t size)
{
    if (!ex)
        return EFS_ERR_INVAL;
    struct efs_inode *p = inode_ptr(ex, ino);
    if (!p)
        return EFS_ERR_NOT_FOUND;
    p->size = size;
    /* Size changes update mtime with full nsec precision (not sec-only). */
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        p->mtime = (uint64_t)ts.tv_sec;
        p->mtime_nsec = (uint32_t)ts.tv_nsec;
    }
    sync_hardlink_attrs(ex, ino, p);
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
    return EFS_OK;
}

int efs_export_set_mtime(struct efs_export *ex, efs_ino_t ino, uint64_t mtime)
{
    return efs_export_set_mtime_ns(ex, ino, mtime, 0);
}

int efs_export_rename(struct efs_export *ex, efs_ino_t ino,
                      efs_ino_t new_parent, const char *new_name)
{
    if (!ex || !new_name)
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
            for (uint64_t i = 0; i < ex->inode_count; i++) {
                if (ex->inodes[i].parent == dst_copy.ino &&
                    ex->inodes[i].ino != dst_copy.ino)
                    return EFS_ERR_NOT_EMPTY;
            }
        }
        /* efs_export_unlink swap-removes the destination entry, which can
         * move or overwrite the entry `src` points to. Re-find it after. */
        efs_export_unlink(ex, dst_copy.ino);
        src = inode_ptr(ex, ino);
        if (!src)
            return EFS_ERR_NOT_FOUND;
    }

    /* Drop old name key before mutating parent/name fields. */
    name_idx_del(ex, src->parent, src->name);
    strncpy(src->name, new_name, EFS_MAX_NAME - 1);
    src->name[EFS_MAX_NAME - 1] = '\0';
    src->parent = new_parent;
    /* Preserve mtime across rename (rsync partial → final); bump ctime only. */
    time_now(&src->ctime);
    name_idx_put(ex, src->parent, src->name, (uint64_t)(src - ex->inodes));
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
        write_u32(f, ino->nlink);
        write_str(f, ino->name);
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
    if (read_u32(f, &version) != 0 || (version != 2 && version != 3)) {
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

    ex->inode_count = 0;
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
        read_u32(f, &ino->nlink);
        read_str(f, ino->name, EFS_MAX_NAME);
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
