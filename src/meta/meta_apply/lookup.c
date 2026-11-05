/* meta_apply/lookup.c: inode and dentry reads, LOOKUP, path resolution. */

#include "apply_internal.h"
#include "lookup.h"
#include "rows.h"

int efs_meta_apply_get_inode(struct efs_kv *kv, efs_ino_t ino,
                             struct efs_meta_row *out)
{
    uint8_t key[EFS_KV_KEY_MAX], val[INO_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    if (!kv || !out || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_decode_result(kv, key, klen, unpack_inode(val, vlen, out));
}

/* Handler reads must observe committed replacements even when an old row
 * exists. Apply readers use the plain getter and never contact a coordinator. */
int inode_read_tx(struct efs_kv *kv, efs_ino_t ino,
                          efs_txn_coord_fn coord, void *ctx,
                          struct efs_txn_pending *pend,
                          struct efs_meta_row *out)
{
    uint8_t key[EFS_KV_KEY_MAX], val[INO_VAL];
    uint32_t klen = 0, vlen = sizeof(val);
    int rc;
    if (!coord)
        return efs_meta_apply_get_inode(kv, ino, out);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &klen);
    if (rc == EFS_OK)
        rc = efs_txn_read_ex(kv, key, klen, coord, ctx, val, &vlen, pend);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_decode_result(kv, key, klen, unpack_inode(val, vlen, out));
}

int efs_meta_apply_get_inode_tx(struct efs_kv *kv, efs_ino_t ino,
                                efs_txn_coord_fn coord, void *ctx,
                                struct efs_meta_row *out)
{
    return inode_read_tx(kv, ino, coord, ctx, NULL, out);
}

int efs_meta_get_writer_view_tx(struct efs_kv *kv, efs_ino_t ino,
                                uint64_t gen, efs_txn_coord_fn coord,
                                void *ctx, struct efs_meta_writer_view *out)
{
    if (!kv || !ino || !gen || !out)
        return EFS_ERR_INVAL;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        struct efs_meta_row before, after;
        struct efs_meta_writer_view view = {0};
        struct efs_fence_history again;
        struct efs_txn_pending pend = {0};
        int moved = 0;
        int rc = inode_read_tx(kv, ino, coord, ctx, &pend, &before);
        if (rc != EFS_OK)
            return rc;
        if (before.generation != gen)
            return EFS_ERR_STALE;
        if (!S_ISREG(before.mode))
            return EFS_ERR_INVAL;
        rc = fence_history_read(kv, ino, gen, EFS_META_FENCE_INODE,
                                  coord, ctx, &pend, &view.history);
        if (rc == EFS_OK)
            rc = fence_history_read(kv, ino, gen, EFS_META_FENCE_INODE,
                                      coord, ctx, &pend, &again);
        if (rc == EFS_OK)
            rc = inode_read_tx(kv, ino, coord, ctx, &pend, &after);
        if (rc == EFS_OK && coord)
            rc = efs_txn_pending_recheck(&pend, coord, ctx, &moved);
        if (rc != EFS_OK)
            return rc;
        if (moved || memcmp(&before, &after, sizeof(before)) ||
            memcmp(&view.history, &again, sizeof(again)))
            continue;
        view.ino = ino;
        view.generation = gen;
        view.authority_epoch = before.content_epoch;
        view.oldest_complete_epoch = before.content_epoch;
        /* Epochs advance by one per shrink. A gap may be a retired fence or
         * legacy physical truncate; neither supplies complete old history.
         * Only a contiguous suffix ending at the current stamp proves it. */
        for (uint32_t i = view.history.count; i > 0; --i) {
            uint64_t epoch = view.history.entries[i - 1].epoch;
            if (epoch > view.authority_epoch)
                return EFS_ERR_PROTO;
            if (epoch != view.oldest_complete_epoch)
                break;
            --view.oldest_complete_epoch;
        }
        *out = view;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

int dent_get(struct efs_kv *kv, uint32_t shard, efs_ino_t parent,
                    const char *name, struct efs_meta_dentry *out)
{
    uint8_t key[EFS_KV_KEY_MAX], val[DENT_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    rc = efs_kv_key_dentry(shard, parent, name, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_decode_result(kv, key, klen, unpack_dentry(val, vlen, out));
}

int efs_meta_capture_dentry_drop(struct efs_kv *kv, const struct efs_meta_row *prow,
                               efs_ino_t parent, const char *name,
                               const struct efs_meta_dentry *expected,
                               struct efs_meta_dentry_drop *d)
{
    struct efs_meta_dentry tomb;
    int rc;

    if (!kv || !prow || !name || !expected || !d || !parent ||
        prow->ino != parent || prow->layout > EFS_META_LAYOUT_HASHED ||
        !expected->ino || expected->type == EFS_META_DENT_TOMBSTONE)
        return EFS_ERR_INVAL;
    memset(d, 0, sizeof(*d));
    d->psh = efs_kv_inode_shard(parent);
    d->hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    rc = efs_kv_key_dentry(d->psh, parent, name, d->k_loc, &d->kl);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(d->hsh, parent, name, d->k_hash, &d->kh);
    if (rc != EFS_OK)
        return rc;
    d->del_loc = prow->layout != EFS_META_LAYOUT_HASHED &&
        !(prow->layout == EFS_META_LAYOUT_SPLITTING && d->kl == d->kh &&
          memcmp(d->k_loc, d->k_hash, d->kl) == 0);
    d->put_tomb = prow->layout == EFS_META_LAYOUT_SPLITTING;
    d->del_hash = prow->layout == EFS_META_LAYOUT_HASHED;
    memset(&tomb, 0, sizeof(tomb));
    tomb.generation = prow->layout_epoch;
    tomb.type = EFS_META_DENT_TOMBSTONE;
    rc = efs_meta_pack_dentry(&tomb, d->v_tomb, sizeof(d->v_tomb));
    if (rc != EFS_OK)
        return rc;
    uint8_t wanted[EFS_META_DENT_BYTES];
    int matched = 0;
    rc = efs_meta_pack_dentry(expected, wanted, sizeof(wanted));
    if (rc != EFS_OK)
        return rc;
    for (unsigned int i = 0; i < 2; ++i) {
        uint8_t *key = i ? d->k_hash : d->k_loc;
        uint8_t *value = i ? d->hash_value : d->loc_value;
        uint32_t kl = i ? d->kh : d->kl;
        uint32_t *len = i ? &d->hash_len : &d->loc_len;
        if (i ? !(d->put_tomb || d->del_hash) : !d->del_loc)
            continue;
        *len = EFS_META_DENT_BYTES;
        rc = efs_kv_get(kv, key, kl, value, len);
        if (rc == EFS_ERR_NOT_FOUND) {
            *len = EFS_TXN_ABSENT;
        } else if (rc != EFS_OK) {
            return rc;
        } else if (*len == sizeof(wanted) && !memcmp(value, wanted, sizeof(wanted))) {
            matched = 1;
        } else {
            /* A hashed tombstone masks even a lingering local copy. */
            return EFS_ERR_STALE;
        }
    }
    /* At least one captured source must still name the originally read inode.
     * Absence or a replacement between lookup and this capture is STALE. */
    return matched ? EFS_OK : EFS_ERR_STALE;
}

/* As dent_get, and when the key is absent a COMMITted EXCL intent is the
 * value (I17: visible at the decision). A negative lookup pays one extra
 * local get of the intent key; the coordinator is asked only when an
 * intent is actually there. NULL coord = plain read. */
int dent_get_tx(struct efs_kv *kv, uint32_t shard, efs_ino_t parent,
                       const char *name, efs_txn_coord_fn coord, void *ctx,
                       struct efs_meta_dentry *out)
{
    uint8_t key[EFS_KV_KEY_MAX], val[DENT_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    rc = dent_get(kv, shard, parent, name, out);
    if (rc != EFS_ERR_NOT_FOUND || !coord)
        return rc;
    rc = efs_kv_key_dentry(shard, parent, name, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = efs_txn_read(kv, key, klen, coord, ctx, val, &vlen);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_decode_result(kv, key, klen, unpack_dentry(val, vlen, out));
}

static int lookup_at(struct efs_kv *kv, efs_ino_t parent, const char *name,
                     efs_txn_coord_fn coord, void *ctx,
                     struct efs_meta_dentry *out)
{
    struct efs_meta_row prow;
    uint32_t hsh;
    int rc;

    if (!kv || !name || !out || parent == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode_tx(kv, parent, coord, ctx, &prow);
    if (rc == EFS_ERR_NOT_FOUND) {
        /* Parent row lives on another Raft group. Try hashed then local. */
        hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
        rc = dent_get_tx(kv, hsh, parent, name, coord, ctx, out);
        if (rc == EFS_OK) {
            if (out->type == EFS_META_DENT_TOMBSTONE)
                return EFS_ERR_NOT_FOUND;
            return EFS_OK;
        }
        if (rc != EFS_ERR_NOT_FOUND)
            return rc;
        return dent_get_tx(kv, efs_kv_inode_shard(parent), parent, name,
                           coord, ctx, out);
    }
    if (rc != EFS_OK)
        return rc;
    return efs_meta_apply_lookup_row_tx(kv, &prow, name, coord, ctx, out);
}

int efs_meta_apply_lookup_row_tx(struct efs_kv *kv,
                                 const struct efs_meta_row *parent_row,
                                 const char *name, efs_txn_coord_fn coord,
                                 void *ctx, struct efs_meta_dentry *out)
{
    if (!kv || !parent_row || !parent_row->ino || !name || !out ||
        parent_row->layout > EFS_META_LAYOUT_HASHED)
        return EFS_ERR_INVAL;
    efs_ino_t parent = parent_row->ino;
    uint32_t hsh;
    int rc;
    if (parent_row->layout == EFS_META_LAYOUT_LOCAL)
        return dent_get_tx(kv, efs_kv_inode_shard(parent), parent, name,
                           coord, ctx, out);
    hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    rc = dent_get_tx(kv, hsh, parent, name, coord, ctx, out);
    if (rc == EFS_OK) {
        if (out->type == EFS_META_DENT_TOMBSTONE)
            return EFS_ERR_NOT_FOUND;
        return EFS_OK;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    if (parent_row->layout == EFS_META_LAYOUT_HASHED)
        return EFS_ERR_NOT_FOUND;
    return dent_get_tx(kv, efs_kv_inode_shard(parent), parent, name, coord,
                       ctx, out);
}

int efs_meta_apply_lookup(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          struct efs_meta_dentry *out)
{
    return lookup_at(kv, parent, name, NULL, NULL, out);
}

int efs_meta_apply_lookup_tx(struct efs_kv *kv, efs_ino_t parent,
                             const char *name, efs_txn_coord_fn coord,
                             void *ctx, struct efs_meta_dentry *out)
{
    return lookup_at(kv, parent, name, coord, ctx, out);
}

int efs_meta_apply_resolve(struct efs_kv *kv, efs_ino_t parent, const char *name,
                           struct efs_meta_dentry *dent, struct efs_meta_row *row)
{
    struct efs_meta_dentry d;
    struct efs_meta_row r;
    int rc;

    rc = efs_meta_apply_lookup(kv, parent, name, &d);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(kv, d.ino, &r);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_ERR_IO; /* I9: dentry without a row is not absence */
    if (rc != EFS_OK)
        return rc;
    if (dent)
        *dent = d;
    if (row)
        *row = r;
    return EFS_OK;
}

/* Handler-side resolve. A COMMITted-but-unresolved dentry or child row
 * is visible (lookup_tx / get_inode_tx). The apply path keeps
 * efs_meta_apply_resolve: it cannot ask a coordinator. Sep 23 freeze:
 * rmdir of d-fcstor011-1-22 returned ENOENT while the name existed,
 * because the creating txn's RESOLVE had not landed. */
int efs_meta_apply_resolve_tx(struct efs_kv *kv, efs_ino_t parent,
                              const char *name, efs_txn_coord_fn coord,
                              void *ctx, struct efs_meta_dentry *dent,
                              struct efs_meta_row *row)
{
    struct efs_meta_dentry d;
    struct efs_meta_row r;
    int rc;

    rc = efs_meta_apply_lookup_tx(kv, parent, name, coord, ctx, &d);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode_tx(kv, d.ino, coord, ctx, &r);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_ERR_IO; /* I9: dentry without a row is not absence */
    if (rc != EFS_OK)
        return rc;
    if (dent)
        *dent = d;
    if (row)
        *row = r;
    return EFS_OK;
}

static void hop_from_row(struct efs_meta_path_hop *h, const struct efs_meta_row *r)
{
    h->ino = r->ino;
    h->generation = r->generation;
    h->mode = r->mode;
    h->uid = r->uid;
    h->gid = r->gid;
}

int efs_meta_apply_lookup_path(struct efs_kv *kv, efs_ino_t start,
                               const char *path, struct efs_meta_path_hop *hops,
                               uint32_t cap, uint32_t *n)
{
    struct efs_meta_row row;
    efs_ino_t cur;
    const char *p;
    uint32_t nh = 0;
    int rc;

    if (!kv || !path || !hops || !n || cap == 0)
        return EFS_ERR_INVAL;
    if (cap > EFS_META_PATH_MAX)
        cap = EFS_META_PATH_MAX;
    cur = start ? start : EFS_ROOT_INO;
    p = path;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        rc = efs_meta_apply_get_inode(kv, cur, &row);
        if (rc != EFS_OK)
            return rc;
        hop_from_row(&hops[0], &row);
        *n = 1;
        return EFS_OK;
    }
    while (*p && nh < cap) {
        char name[EFS_MAX_NAME];
        size_t nlen;
        const char *s = p;
        struct efs_meta_row child;
        int more;

        while (*p && *p != '/')
            p++;
        nlen = (size_t)(p - s);
        if (nlen == 0)
            break;
        if (nlen >= EFS_MAX_NAME)
            return EFS_ERR_NAMETOOLONG;
        memcpy(name, s, nlen);
        name[nlen] = '\0';
        while (*p == '/')
            p++;
        rc = efs_meta_apply_resolve(kv, cur, name, NULL, &child);
        if (rc != EFS_OK)
            return rc;
        more = (*p != '\0');
        /* A file or symlink cannot be an intermediate: POSIX walks only
         * through directories, and following a symlink is the caller's
         * job after this returns. Stopping with the non-dir as a hop
         * would look like a successful batch the caller then resumes. */
        if (more && !S_ISDIR(child.mode))
            return EFS_ERR_INVAL;
        hop_from_row(&hops[nh], &child);
        nh++;
        cur = child.ino;
    }
    *n = nh;
    return nh ? EFS_OK : EFS_ERR_INVAL;
}
