#include "efs/meta_apply.h"
#include "efs/kv_key.h"
#include "efs/session.h"
#include "efs/checksum.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define INO_VAL  EFS_META_INO_BYTES
#define DENT_VAL 20
#define ALLOC_VAL 8
#define LANE_VAL 48
#define CHUNK_HDR 20
#define CHUNK_VAL (CHUNK_HDR + 4 * EFS_NUM_FRAGMENTS + \
                   EFS_HASH_SIZE * EFS_NUM_FRAGMENTS)

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void be64(uint8_t *p, uint64_t v)
{
    be32(p, (uint32_t)(v >> 32));
    be32(p + 4, (uint32_t)v);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)rd32(p) << 32) | rd32(p + 4);
}

static uint64_t max_u64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

/* A write lane's high-water marks — the values stat() reduces over (§7.3).
 *
 * The first 24 bytes are deliberately the same triple a transaction's
 * commutative reduction carries (`struct efs_txn_reduce`), so a pending
 * committed reduction folds into a materialized lane without either side
 * knowing the other's layout. Anything after that is the lane owner's.
 *
 * `mtime_gen` records which utimens generation `max_mtime` belongs to. Only
 * utimens can move a timestamp backwards, so it bumps the inode row's
 * generation and a stat ignores lane mtimes stamped under an older one —
 * which invalidates every stale lane mtime at once instead of rewriting 64
 * lanes. `fenced_epoch` is the content-epoch fence a truncate installs. */
struct lane_rec {
    uint64_t max_end;
    uint64_t max_mtime;
    uint64_t max_ctime;
    uint64_t seq;
    uint64_t fenced_epoch;
    uint64_t mtime_gen;
};

static void pack_lane(uint8_t *p, const struct lane_rec *l)
{
    be64(p + 0, l->max_end);
    be64(p + 8, l->max_mtime);
    be64(p + 16, l->max_ctime);
    be64(p + 24, l->seq);
    be64(p + 32, l->fenced_epoch);
    be64(p + 40, l->mtime_gen);
}

/* Tolerates a record carrying only the reduce triple: a lane whose only
 * writes so far arrived as reductions has no owner-written tail. */
static int unpack_lane(const uint8_t *p, uint32_t n, struct lane_rec *l)
{
    memset(l, 0, sizeof(*l));
    if (n < 24)
        return EFS_ERR_PROTO;
    l->max_end = rd64(p + 0);
    l->max_mtime = rd64(p + 8);
    l->max_ctime = rd64(p + 16);
    if (n >= 32)
        l->seq = rd64(p + 24);
    if (n >= 40)
        l->fenced_epoch = rd64(p + 32);
    if (n >= 48)
        l->mtime_gen = rd64(p + 40);
    return EFS_OK;
}

/* Dir-lane stamp for a HASHED/SPLITTING mutation. Same record as a file
 * write lane — MAX mtime/ctime + seq — living on the dentry shard so a
 * create does not bounce back to the directory's home leader (§7.4). */
static int dir_lane_stamp(struct efs_kv *kv, const struct efs_meta_row *dir,
                          const char *name, uint64_t now,
                          uint8_t *k_ln, uint32_t *kl, uint8_t *v_ln)
{
    uint8_t lane = efs_kv_dir_lane(name);
    uint8_t old[LANE_VAL];
    uint32_t vn = sizeof(old);
    struct lane_rec ln;
    int rc;

    *kl = 0;
    rc = efs_kv_key_lane(efs_kv_lane_shard(dir->ino, lane), dir->ino,
                         dir->generation, lane, k_ln, kl);
    if (rc != EFS_OK)
        return rc;
    memset(&ln, 0, sizeof(ln));
    rc = efs_kv_get(kv, k_ln, *kl, old, &vn);
    if (rc == EFS_OK) {
        rc = unpack_lane(old, vn, &ln);
        if (rc != EFS_OK)
            return rc;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    ln.max_mtime = max_u64(ln.max_mtime, now);
    ln.max_ctime = max_u64(ln.max_ctime, now);
    ln.seq++;
    ln.mtime_gen = dir->mtime_gen;
    pack_lane(v_ln, &ln);
    return EFS_OK;
}

static void pack_inode(uint8_t *p, const struct efs_meta_row *r)
{
    be64(p + 0, r->ino);
    be64(p + 8, r->generation);
    be32(p + 16, r->mode);
    be32(p + 20, r->nlink);
    be64(p + 24, r->parent);
    be64(p + 32, r->base_size);
    be64(p + 40, r->active_lanes);
    be64(p + 48, r->content_epoch);
    p[56] = r->layout;
    memset(p + 57, 0, 7);
    be64(p + 64, r->layout_epoch);
    be64(p + 72, r->used_shards);
    be32(p + 80, r->uid);
    be32(p + 84, r->gid);
    be64(p + 88, r->base_mtime);
    be64(p + 96, r->base_atime);
    be64(p + 104, r->base_ctime);
    be64(p + 112, r->mtime_gen);
}

static int unpack_inode(const uint8_t *p, uint32_t n, struct efs_meta_row *r)
{
    if (!p || !r || n < INO_VAL)
        return EFS_ERR_PROTO;
    memset(r, 0, sizeof(*r));
    r->ino = rd64(p + 0);
    r->generation = rd64(p + 8);
    r->mode = rd32(p + 16);
    r->nlink = rd32(p + 20);
    r->parent = rd64(p + 24);
    r->base_size = rd64(p + 32);
    r->active_lanes = rd64(p + 40);
    r->content_epoch = rd64(p + 48);
    r->layout = p[56];
    r->layout_epoch = rd64(p + 64);
    r->used_shards = rd64(p + 72);
    r->uid = rd32(p + 80);
    r->gid = rd32(p + 84);
    r->base_mtime = rd64(p + 88);
    r->base_atime = rd64(p + 96);
    r->base_ctime = rd64(p + 104);
    r->mtime_gen = rd64(p + 112);
    return EFS_OK;
}

static void pack_dentry(uint8_t *p, const struct efs_meta_dentry *d)
{
    be64(p + 0, d->ino);
    be64(p + 8, d->generation);
    be32(p + 16, d->type);
}

static int unpack_dentry(const uint8_t *p, uint32_t n, struct efs_meta_dentry *d)
{
    if (!p || !d || n < DENT_VAL)
        return EFS_ERR_PROTO;
    memset(d, 0, sizeof(*d));
    d->ino = rd64(p + 0);
    d->generation = rd64(p + 8);
    d->type = rd32(p + 16);
    return EFS_OK;
}

static int kv_get_copy(struct efs_kv *kv, const uint8_t *key, uint32_t klen,
                       uint8_t *buf, uint32_t cap, uint32_t *out_len)
{
    uint32_t n = cap;
    int rc;

    rc = efs_kv_get(kv, key, klen, buf, &n);
    if (rc != EFS_OK)
        return rc;
    if (out_len)
        *out_len = n;
    return EFS_OK;
}

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
    return unpack_inode(val, vlen, out);
}

static int dent_get(struct efs_kv *kv, uint32_t shard, efs_ino_t parent,
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
    return unpack_dentry(val, vlen, out);
}

int efs_meta_apply_lookup(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          struct efs_meta_dentry *out)
{
    struct efs_meta_row prow;
    uint32_t hsh;
    int rc;

    if (!kv || !name || !out || parent == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, parent, &prow);
    if (rc != EFS_OK)
        return rc;
    if (prow.layout == EFS_META_LAYOUT_LOCAL)
        return dent_get(kv, efs_kv_inode_shard(parent), parent, name, out);
    hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    rc = dent_get(kv, hsh, parent, name, out);
    if (rc == EFS_OK) {
        if (out->type == EFS_META_DENT_TOMBSTONE)
            return EFS_ERR_NOT_FOUND;
        return EFS_OK;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    if (prow.layout == EFS_META_LAYOUT_HASHED)
        return EFS_ERR_NOT_FOUND;
    return dent_get(kv, efs_kv_inode_shard(parent), parent, name, out);
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

static int load_alloc(struct efs_kv *kv, uint32_t shard, efs_ino_t *next)
{
    uint8_t key[EFS_KV_KEY_MAX], val[ALLOC_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    rc = efs_kv_key_alloc(shard, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc == EFS_ERR_NOT_FOUND) {
        *next = shard ? (efs_ino_t)shard : (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
        if (*next == EFS_ROOT_INO)
            *next += (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
        return EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    if (vlen < ALLOC_VAL)
        return EFS_ERR_PROTO;
    *next = rd64(val);
    return EFS_OK;
}

static int load_window(struct efs_kv *kv, const struct efs_opid *op,
                       struct efs_opid_window *w)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_OPID_VAL_MAX];
    uint32_t klen = 0, vlen;
    uint32_t shard;
    int rc;

    shard = efs_kv_session_shard(op->client_uuid);
    rc = efs_kv_key_opid(shard, op->client_uuid, op->session_epoch, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = efs_kv_get(kv, key, klen, val, &vlen);
    if (rc == EFS_ERR_NOT_FOUND) {
        efs_opid_window_init(w, op->client_uuid, op->session_epoch);
        return EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    return efs_opid_window_unpack(w, val, vlen);
}

int efs_meta_apply_init(struct efs_kv *kv, uint64_t now)
{
    struct efs_meta_row root;
    uint8_t k_ino[EFS_KV_KEY_MAX], k_alloc[EFS_KV_KEY_MAX];
    uint8_t v_ino[INO_VAL], v_alloc[ALLOC_VAL];
    uint32_t lk = 0, ak = 0;
    struct efs_kv_item it[2];
    uint32_t shard = efs_kv_inode_shard(EFS_ROOT_INO);
    int rc;

    if (!kv)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, EFS_ROOT_INO, &root);
    if (rc == EFS_OK)
        return EFS_OK;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    memset(&root, 0, sizeof(root));
    root.ino = EFS_ROOT_INO;
    root.generation = 1;
    root.mode = S_IFDIR | 0755;
    root.nlink = 2;
    root.parent = EFS_ROOT_INO;
    root.base_mtime = now;
    root.base_atime = now;
    root.base_ctime = now;
    pack_inode(v_ino, &root);
    be64(v_alloc, EFS_ROOT_INO + (efs_ino_t)(1u << EFS_KV_SHARD_BITS));
    rc = efs_kv_key_inode(shard, EFS_ROOT_INO, k_ino, &lk);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(shard, k_alloc, &ak);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[0].op = EFS_KV_PUT;
    it[0].key = k_ino;
    it[0].klen = lk;
    it[0].val = v_ino;
    it[0].vlen = INO_VAL;
    it[1].op = EFS_KV_PUT;
    it[1].key = k_alloc;
    it[1].klen = ak;
    it[1].val = v_alloc;
    it[1].vlen = ALLOC_VAL;
    return efs_kv_batch(kv, it, 2);
}

int efs_meta_pack_inode(const struct efs_meta_row *r, uint8_t *out, uint32_t cap)
{
    if (!r || !out || cap < INO_VAL)
        return EFS_ERR_INVAL;
    pack_inode(out, r);
    return EFS_OK;
}

int efs_meta_pack_dentry(const struct efs_meta_dentry *d, uint8_t *out,
                         uint32_t cap)
{
    if (!d || !out || cap < DENT_VAL)
        return EFS_ERR_INVAL;
    pack_dentry(out, d);
    return EFS_OK;
}

int efs_meta_unpack_dentry(const uint8_t *p, uint32_t n, struct efs_meta_dentry *d)
{
    return unpack_dentry(p, n, d);
}

int efs_meta_apply_peek_alloc(struct efs_kv *kv, uint32_t shard, efs_ino_t *next)
{
    if (!kv || !next)
        return EFS_ERR_INVAL;
    return load_alloc(kv, shard, next);
}

static int create_file_batch(struct efs_kv *kv, const struct efs_meta_attrs *at,
                             efs_ino_t parent, uint32_t mode,
                             const char *name, const struct efs_opid *op,
                             efs_ino_t *out)
{
    struct efs_meta_dentry dent;
    struct efs_meta_row row, parent_row;
    struct efs_opid_window win;
    struct efs_opid_reply rep;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_opid[EFS_KV_KEY_MAX];
    uint8_t v_dent[DENT_VAL], v_ino[INO_VAL], v_alloc[ALLOC_VAL];
    uint8_t v_opid[EFS_OPID_VAL_MAX];
    uint32_t kd = 0, ki = 0, ka = 0, ko = 0, vo = sizeof(v_opid);
    struct efs_kv_item it[6];
    uint32_t n = 0, shard;
    efs_ino_t next = 0, ino;
    uint8_t k_par[EFS_KV_KEY_MAX], v_par[INO_VAL];
    uint8_t k_ln[EFS_KV_KEY_MAX], v_ln[LANE_VAL];
    uint32_t kp = 0, kln = 0;
    uint64_t bit;
    int rc;
    int touch_parent = 0;
    int stamp_lane = 0;

    if (!kv || !name || !at || parent == 0)
        return EFS_ERR_INVAL;
    if ((mode & S_IFMT) == S_IFDIR)
        return EFS_ERR_INVAL; /* MKDIR is a 2-shard txn; not this helper */
    rc = efs_meta_apply_get_inode(kv, parent, &parent_row);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(parent_row.mode))
        return EFS_ERR_INVAL;

    if (op) {
        int hit;

        rc = load_window(kv, op, &win);
        if (rc != EFS_OK)
            return rc;
        hit = efs_opid_lookup(&win, op, &rep);
        if (hit < 0)
            return hit;
        if (hit) {
            if (out)
                *out = rep.ino;
            return rep.rc;
        }
    }

    rc = efs_meta_apply_lookup(kv, parent, name, &dent);
    if (rc == EFS_OK)
        return EFS_ERR_EXIST;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;

    shard = efs_kv_dentry_shard(parent, name, parent_row.layout);
    rc = load_alloc(kv, shard, &next);
    if (rc != EFS_OK)
        return rc;
    ino = next;
    if (efs_kv_inode_shard(ino) != shard)
        return EFS_ERR_PROTO;
    next = ino + (efs_ino_t)(1u << EFS_KV_SHARD_BITS);

    memset(&row, 0, sizeof(row));
    row.ino = ino;
    row.generation = 1;
    row.mode = mode;
    row.nlink = 1;
    row.parent = parent;
    row.uid = at->uid;
    row.gid = at->gid;
    row.base_mtime = at->now;
    row.base_atime = at->now;
    row.base_ctime = at->now;
    memset(&dent, 0, sizeof(dent));
    dent.ino = ino;
    dent.generation = row.generation;
    dent.type = mode & S_IFMT;
    pack_inode(v_ino, &row);
    pack_dentry(v_dent, &dent);
    be64(v_alloc, next);

    rc = efs_kv_key_dentry(shard, parent, name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(shard, ino, k_ino, &ki);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(shard, k_alloc, &ka);
    if (rc != EFS_OK)
        return rc;

    /* POSIX: adding an entry moves the containing directory's mtime and
     * ctime. For a LOCAL directory the parent row is on this very shard, so
     * it rides the same atomic batch and costs nothing. For a spread
     * directory it deliberately does NOT go here — routing every create back
     * to the parent's shard is the hotspot the spread exists to remove, so
     * those times live in a per-dentry-shard dir lane (§7.4). */
    if (parent_row.layout == EFS_META_LAYOUT_LOCAL) {
        parent_row.base_mtime = max_u64(parent_row.base_mtime, at->now);
        parent_row.base_ctime = max_u64(parent_row.base_ctime, at->now);
        touch_parent = 1;
    } else {
        bit = 1ull << efs_kv_dir_lane(name);
        if ((parent_row.used_shards & bit) == 0) {
            parent_row.used_shards |= bit;
            touch_parent = 1;
        }
        rc = dir_lane_stamp(kv, &parent_row, name, at->now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
    }
    if (touch_parent) {
        pack_inode(v_par, &parent_row);
        rc = efs_kv_key_inode(efs_kv_inode_shard(parent), parent, k_par, &kp);
        if (rc != EFS_OK)
            return rc;
    }

    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dent;
    it[n].klen = kd;
    it[n].val = v_dent;
    it[n].vlen = DENT_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_alloc;
    it[n].klen = ka;
    it[n].val = v_alloc;
    it[n].vlen = ALLOC_VAL;
    n++;
    if (touch_parent) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_par;
        it[n].klen = kp;
        it[n].val = v_par;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_lane) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ln;
        it[n].klen = kln;
        it[n].val = v_ln;
        it[n].vlen = LANE_VAL;
        n++;
    }

    if (op) {
        memset(&rep, 0, sizeof(rep));
        rep.rc = EFS_OK;
        rep.ino = ino;
        rc = efs_opid_complete(&win, op, &rep);
        if (rc != EFS_OK)
            return rc;
        vo = sizeof(v_opid);
        rc = efs_opid_window_pack(&win, v_opid, &vo);
        if (rc != EFS_OK)
            return rc;
        rc = efs_kv_key_opid(efs_kv_session_shard(op->client_uuid),
                             op->client_uuid, op->session_epoch, k_opid, &ko);
        if (rc != EFS_OK)
            return rc;
        it[n].op = EFS_KV_PUT;
        it[n].key = k_opid;
        it[n].klen = ko;
        it[n].val = v_opid;
        it[n].vlen = vo;
        n++;
    }

    rc = efs_kv_batch(kv, it, n);
    if (rc != EFS_OK)
        return rc;
    if (out)
        *out = ino;
    return EFS_OK;
}

int efs_meta_apply_create_file(struct efs_kv *kv, const struct efs_meta_attrs *at,
                               efs_ino_t parent, uint32_t mode,
                               const char *name, efs_ino_t *out)
{
    return create_file_batch(kv, at, parent, mode, name, NULL, out);
}

int efs_meta_apply_create_file_op(struct efs_kv *kv, const struct efs_opid *op,
                                  const struct efs_meta_attrs *at,
                                  efs_ino_t parent, uint32_t mode, const char *name,
                                  efs_ino_t *out)
{
    if (!op)
        return EFS_ERR_INVAL;
    return create_file_batch(kv, at, parent, mode, name, op, out);
}

int efs_meta_apply_unlink(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          uint64_t now)
{
    struct efs_meta_dentry dent, tomb;
    struct efs_meta_row row, prow;
    uint8_t k_loc[EFS_KV_KEY_MAX], k_hash[EFS_KV_KEY_MAX];
    uint8_t k_ino[EFS_KV_KEY_MAX], v_ino[INO_VAL], v_tomb[DENT_VAL];
    uint8_t k_par[EFS_KV_KEY_MAX], v_par[INO_VAL];
    uint8_t k_ln[EFS_KV_KEY_MAX], v_ln[LANE_VAL];
    uint32_t kl = 0, kh = 0, ki = 0, kp = 0, kln = 0;
    struct efs_kv_item it[6];
    uint32_t n = 0, psh, hsh;
    int rc, held = 0;
    int touch_parent = 0;
    int stamp_lane = 0;

    rc = efs_meta_apply_resolve(kv, parent, name, &dent, &row);
    if (rc != EFS_OK)
        return rc;
    if (S_ISDIR(row.mode))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, parent, &prow);
    if (rc != EFS_OK)
        return rc;
    psh = efs_kv_inode_shard(parent);
    hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    rc = efs_kv_key_dentry(psh, parent, name, k_loc, &kl);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(hsh, parent, name, k_hash, &kh);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    /* A lane-0 name during SPLITTING has k_hash == k_loc, and the tombstone
     * below is what has to survive; dropping the redundant delete keeps that
     * from depending on the order a batch applies its items in. In a LOCAL
     * directory the keys alias too, but nothing rewrites the key afterwards,
     * so there the delete is the whole operation. */
    if (prow.layout != EFS_META_LAYOUT_HASHED &&
        !(prow.layout == EFS_META_LAYOUT_SPLITTING &&
          kl == kh && memcmp(k_loc, k_hash, kl) == 0)) {
        it[n].op = EFS_KV_DEL;
        it[n].key = k_loc;
        it[n].klen = kl;
        n++;
    }
    if (prow.layout == EFS_META_LAYOUT_SPLITTING) {
        memset(&tomb, 0, sizeof(tomb));
        tomb.generation = prow.layout_epoch;
        tomb.type = EFS_META_DENT_TOMBSTONE;
        pack_dentry(v_tomb, &tomb);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_hash;
        it[n].klen = kh;
        it[n].val = v_tomb;
        it[n].vlen = DENT_VAL;
        n++;
    } else if (prow.layout == EFS_META_LAYOUT_HASHED) {
        it[n].op = EFS_KV_DEL;
        it[n].key = k_hash;
        it[n].klen = kh;
        n++;
    }
    if (row.nlink <= 1) {
        held = efs_lease_any(kv, row.ino, row.generation);
        if (held < 0)
            return held;
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
        if (held) {
            row.nlink = 0;
            pack_inode(v_ino, &row);
            it[n].op = EFS_KV_PUT;
            it[n].key = k_ino;
            it[n].klen = ki;
            it[n].val = v_ino;
            it[n].vlen = INO_VAL;
            n++;
        } else {
            it[n].op = EFS_KV_DEL;
            it[n].key = k_ino;
            it[n].klen = ki;
            n++;
        }
    }
    /* Same split as create: LOCAL times ride the parent row; spread times
     * live on the dentry shard's dir lane so unlink does not re-serialize
     * on the directory's home leader. */
    if (prow.layout == EFS_META_LAYOUT_LOCAL) {
        prow.base_mtime = max_u64(prow.base_mtime, now);
        prow.base_ctime = max_u64(prow.base_ctime, now);
        touch_parent = 1;
    } else {
        rc = dir_lane_stamp(kv, &prow, name, now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
    }
    if (touch_parent) {
        pack_inode(v_par, &prow);
        rc = efs_kv_key_inode(psh, parent, k_par, &kp);
        if (rc != EFS_OK)
            return rc;
        it[n].op = EFS_KV_PUT;
        it[n].key = k_par;
        it[n].klen = kp;
        it[n].val = v_par;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_lane) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ln;
        it[n].klen = kln;
        it[n].val = v_ln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    return efs_kv_batch(kv, it, n);
}

int efs_meta_apply_setattr(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                           const struct efs_meta_setattr *sa)
{
    struct efs_meta_row row;
    uint8_t key[EFS_KV_KEY_MAX], val[INO_VAL];
    uint32_t klen = 0;
    int rc;

    if (!kv || !sa || ino == 0)
        return EFS_ERR_INVAL;
    if (sa->mask & ~(EFS_META_SET_MODE | EFS_META_SET_UID | EFS_META_SET_GID))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (sa->expect_gen != 0 && sa->expect_gen != row.generation)
        return EFS_ERR_STALE;
    if (sa->mask & EFS_META_SET_MODE)
        row.mode = (row.mode & ~07777u) | (sa->mode & 07777u);
    if (sa->mask & EFS_META_SET_UID)
        row.uid = sa->uid;
    if (sa->mask & EFS_META_SET_GID)
        row.gid = sa->gid;
    /* A metadata change stamps ctime and leaves mtime alone. MAX-clamped, so
     * a backwards CLOCK_REALTIME cannot walk ctime back, and so re-applying
     * the same committed entry is a no-op. */
    row.base_ctime = max_u64(row.base_ctime, now);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &klen);
    if (rc != EFS_OK)
        return rc;
    pack_inode(val, &row);
    return efs_kv_put(kv, key, klen, val, INO_VAL);
}

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
 * of the local pass (every unmigrated name would come back twice). */

#define DENTRY_KEY_PREFIX 11 /* shard + kind + parent; name follows */
#define READDIR_SRC_LOCAL EFS_META_LANES /* the pre-split source */

struct dir_scan {
    struct efs_meta_dir_ent *out;
    uint32_t max;
    uint32_t n;
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

    if (klen <= DENTRY_KEY_PREFIX)
        return 0; /* the prefix itself is not an entry */
    nl = klen - DENTRY_KEY_PREFIX;
    if (nl >= EFS_MAX_NAME) {
        s->rc = EFS_ERR_PROTO;
        return EFS_ERR_PROTO;
    }
    if (s->n >= s->max) {
        s->full = 1; /* stop; the cursor resumes in this same source */
        return 1;
    }
    memcpy(s->out[s->n].name, key + DENTRY_KEY_PREFIX, nl);
    s->out[s->n].name[nl] = 0;
    if (unpack_dentry(val, vlen, &s->out[s->n].d) != EFS_OK) {
        s->rc = EFS_ERR_PROTO;
        return EFS_ERR_PROTO;
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

/* During SPLITTING the hashed side is authoritative for a name. */
static int hashed_side_claims(struct efs_kv *kv, efs_ino_t dir, const char *name,
                              int *claimed)
{
    struct efs_meta_dentry d;
    int rc;

    *claimed = 0;
    /* A lane-0 name has no separate hashed copy to defer to — the record in
     * hand IS its hashed record. Asking the KV would find that same record
     * and drop the name from the listing. */
    if (efs_kv_dir_lane(name) == 0)
        return EFS_OK;
    rc = dent_get(kv, efs_kv_dentry_shard(dir, name, EFS_META_LAYOUT_HASHED),
                  dir, name, &d);
    if (rc == EFS_OK) {
        *claimed = 1;
        return EFS_OK;
    }
    if (rc == EFS_ERR_NOT_FOUND) {
        *claimed = 0;
        return EFS_OK;
    }
    return rc;
}

int efs_meta_apply_readdir(struct efs_kv *kv, efs_ino_t dir,
                           struct efs_meta_dir_cursor *cur,
                           struct efs_meta_dir_ent *out, uint32_t max,
                           uint32_t *n)
{
    struct efs_meta_row row;
    struct dir_scan s;
    uint32_t last_src;
    int rc;

    if (!kv || !cur || !out || !n || max == 0 || dir == 0)
        return EFS_ERR_INVAL;
    *n = 0;
    if (cur->done)
        return EFS_OK;
    rc = efs_meta_apply_get_inode(kv, dir, &row);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(row.mode))
        return EFS_ERR_INVAL; /* as create_file does for a non-dir parent */

    last_src = row.layout == EFS_META_LAYOUT_LOCAL ? 0 :
               row.layout == EFS_META_LAYOUT_HASHED ? EFS_META_LANES - 1 :
               READDIR_SRC_LOCAL;

    memset(&s, 0, sizeof(s));
    s.out = out;
    s.max = max;
    while (cur->src <= last_src && s.n < max) {
        uint32_t shard, base = s.n;
        int local_pass = 0;

        if (row.layout == EFS_META_LAYOUT_LOCAL) {
            shard = efs_kv_inode_shard(dir);
        } else if (cur->src == READDIR_SRC_LOCAL) {
            shard = efs_kv_inode_shard(dir);
            local_pass = 1;
        } else {
            /* Lane 0's key range is the pre-split local range, which the
             * local pass below already covers while the split is running. */
            if (cur->src == 0 && row.layout == EFS_META_LAYOUT_SPLITTING) {
                cur->src++;
                cur->name[0] = 0;
                continue;
            }
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
        rc = dir_scan_one(kv, shard, dir, cur->name, &s);
        if (rc != EFS_OK)
            return rc;
        if (local_pass) {
            uint32_t i, keep = base;

            /* Only this source's entries; earlier sources are already final. */
            for (i = base; i < s.n; i++) {
                int claimed = 0;

                rc = hashed_side_claims(kv, dir, out[i].name, &claimed);
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
            /* More remains in this source, so do not advance it. Returning
             * an empty page while entries remain would make any caller that
             * stops at *n == 0 miss them, so keep scanning until there is
             * something to deliver; the cursor advanced, so this terminates.
             * The only way to get here empty is a full page of names the
             * hashed side already owns. */
            if (s.n > 0)
                break;
            continue;
        }
        /* This source is exhausted: advance with a fresh name position. */
        cur->src++;
        cur->name[0] = 0;
    }
    if (cur->src > last_src)
        cur->done = 1;
    *n = s.n;
    return EFS_OK;
}

int efs_meta_apply_reclaim(struct efs_kv *kv, efs_ino_t ino)
{
    struct efs_meta_row row;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint32_t ki = 0;
    int rc, held;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (row.nlink != 0)
        return EFS_ERR_BUSY;
    held = efs_lease_any(kv, ino, row.generation);
    if (held < 0)
        return held;
    if (held)
        return EFS_ERR_BUSY;
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_del(kv, k_ino, ki);
}

static void pack_chunk(uint8_t *p, const struct efs_meta_chunk *ch)
{
    int i;

    be64(p + 0, ch->generation);
    be32(p + 8, ch->coding_profile_id);
    be64(p + 12, ch->content_epoch);
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        be32(p + CHUNK_HDR + (uint32_t)i * 4u, ch->nodes[i]);
    memcpy(p + CHUNK_HDR + 4 * EFS_NUM_FRAGMENTS, ch->checksums,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
}

static int unpack_chunk(const uint8_t *p, uint32_t n, struct efs_meta_chunk *ch)
{
    int i;

    if (!p || !ch || n < CHUNK_VAL)
        return EFS_ERR_PROTO;
    memset(ch, 0, sizeof(*ch));
    ch->generation = rd64(p + 0);
    ch->coding_profile_id = rd32(p + 8);
    ch->content_epoch = rd64(p + 12);
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        ch->nodes[i] = rd32(p + CHUNK_HDR + (uint32_t)i * 4u);
    memcpy(ch->checksums, p + CHUNK_HDR + 4 * EFS_NUM_FRAGMENTS,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
    return EFS_OK;
}

uint64_t efs_meta_candidate_gen(const uint8_t uuid[16], uint32_t session_epoch,
                                uint64_t seq, uint32_t chunk_index,
                                uint32_t retry)
{
    uint8_t in[16 + 4 + 8 + 4 + 4];
    uint8_t out[EFS_HASH_SIZE];
    uint64_t g;

    if (!uuid)
        return 1;
    memcpy(in, uuid, 16);
    be32(in + 16, session_epoch);
    be64(in + 20, seq);
    be32(in + 28, chunk_index);
    be32(in + 32, retry);
    efs_hash(in, sizeof(in), out);
    g = rd64(out);
    return g ? g : 1;
}

static int evidence_ok(const struct efs_meta_pub *p)
{
    int i, j;

    if (!p || p->candidate_gen == 0)
        return EFS_ERR_INVAL;
    if (p->coding_profile_id != EFS_META_PROFILE_K2F1)
        return EFS_ERR_STALE;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (p->ch.nodes[i] == 0)
            return EFS_ERR_INVAL;
        for (j = 0; j < i; j++) {
            if (p->ch.nodes[i] == p->ch.nodes[j])
                return EFS_ERR_INVAL;
        }
    }
    return EFS_OK;
}

int efs_meta_apply_publish(struct efs_kv *kv, const struct efs_meta_pub *p)
{
    struct efs_meta_row row;
    struct efs_meta_chunk stored, got;
    uint8_t lane;
    uint32_t lsh, ish;
    uint8_t k_ch[EFS_KV_KEY_MAX], k_ln[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t v_ch[CHUNK_VAL], v_ln[LANE_VAL], v_ino[INO_VAL];
    uint8_t old_ln[LANE_VAL], old_ch[CHUNK_VAL];
    uint32_t kc = 0, kl = 0, ki = 0, vn;
    uint64_t committed = 0;
    struct lane_rec ln;
    struct efs_kv_item it[3];
    uint32_t n = 0;
    int touch_inode = 0;
    int rc;

    if (!kv || !p || p->ino == 0)
        return EFS_ERR_INVAL;
    rc = evidence_ok(p);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(kv, p->ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (p->content_epoch < row.content_epoch)
        return EFS_ERR_STALE;
    lane = (uint8_t)(p->chunk_index % EFS_META_LANES);
    lsh = efs_kv_lane_shard(p->ino, lane);
    ish = efs_kv_inode_shard(p->ino);
    rc = efs_kv_key_chunk(lsh, p->ino, row.generation, lane, p->chunk_index,
                          k_ch, &kc);
    if (rc == EFS_OK)
        rc = efs_kv_key_lane(lsh, p->ino, row.generation, lane, k_ln, &kl);
    if (rc != EFS_OK)
        return rc;
    vn = sizeof(old_ch);
    rc = efs_kv_get(kv, k_ch, kc, old_ch, &vn);
    if (rc == EFS_OK) {
        rc = unpack_chunk(old_ch, vn, &got);
        if (rc != EFS_OK)
            return rc;
        committed = got.generation;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    if (committed == p->candidate_gen)
        return EFS_OK;
    if (p->expected_gen != committed)
        return EFS_ERR_STALE;
    vn = sizeof(old_ln);
    rc = efs_kv_get(kv, k_ln, kl, old_ln, &vn);
    if (rc == EFS_OK) {
        rc = unpack_lane(old_ln, vn, &ln);
        if (rc != EFS_OK)
            return rc;
    } else if (rc == EFS_ERR_NOT_FOUND) {
        memset(&ln, 0, sizeof(ln));
    } else {
        return rc;
    }
    if (p->content_epoch < ln.fenced_epoch)
        return EFS_ERR_STALE;
    ln.max_end = max_u64(ln.max_end, p->new_size);
    /* A write updates mtime AND ctime, and both live here rather than on the
     * inode row so that a million writers never touch the inode's leader.
     * MAX-clamped: CLOCK_REALTIME can step backwards, and re-applying a
     * committed entry must not move anything. */
    if (ln.mtime_gen < row.mtime_gen) {
        /* A utimens has invalidated this lane's mtime since it was stamped,
         * so it is not a value to take a MAX against — it is stale. */
        ln.max_mtime = p->now;
        ln.mtime_gen = row.mtime_gen;
    } else {
        ln.max_mtime = max_u64(ln.max_mtime, p->now);
    }
    ln.max_ctime = max_u64(ln.max_ctime, p->now);
    ln.seq++;
    stored = p->ch;
    stored.generation = p->candidate_gen;
    stored.coding_profile_id = p->coding_profile_id;
    stored.content_epoch = p->content_epoch;
    pack_chunk(v_ch, &stored);
    pack_lane(v_ln, &ln);

    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ch;
    it[n].klen = kc;
    it[n].val = v_ch;
    it[n].vlen = CHUNK_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ln;
    it[n].klen = kl;
    it[n].val = v_ln;
    it[n].vlen = LANE_VAL;
    n++;
    /* First use of a lane has to register it in the inode row's bitmap: that
     * bitmap is the whole collect set for stat(), so a lane nobody recorded
     * is a lane stat() never reads. It happens at most 64 times in a file's
     * life, which is why it is not a rate-proportional cost — but when the
     * lane is not the inode's own shard it is a genuine two-shard write, and
     * once shards are separate Raft groups it must go through §7.2 rather
     * than ride this batch. */
    if ((row.active_lanes & (1ULL << lane)) == 0) {
        row.active_lanes |= 1ULL << lane;
        touch_inode = 1;
    }
    if (lsh == ish && p->new_size > row.base_size) {
        row.base_size = p->new_size;
        touch_inode = 1;
    }
    if (touch_inode) {
        pack_inode(v_ino, &row);
        rc = efs_kv_key_inode(ish, p->ino, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ino;
        it[n].klen = ki;
        it[n].val = v_ino;
        it[n].vlen = INO_VAL;
        n++;
    }
    return efs_kv_batch(kv, it, n);
}

/* Reads a lane's sequence number. Absent is seq 0, which is a real value:
 * a lane with no record has nothing to contribute and nothing to change. */
static int lane_seq_get(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                        uint8_t lane, uint64_t *seq)
{
    uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
    uint32_t kl = 0, vn = sizeof(val);
    struct lane_rec ln;
    int rc;

    *seq = 0;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    if (rc != EFS_OK)
        return rc;
    vn = sizeof(val);
    rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    rc = unpack_lane(val, vn, &ln);
    if (rc != EFS_OK)
        return rc;
    *seq = ln.seq;
    return EFS_OK;
}

int efs_meta_apply_getattr(struct efs_kv *kv, efs_ino_t ino,
                           efs_txn_coord_fn coord, void *ctx,
                           struct efs_meta_stat *out)
{
    uint64_t seq1[EFS_META_LANES], seq2[EFS_META_LANES];
    uint8_t lanes[EFS_META_LANES];
    uint32_t tries;

    if (!kv || !coord || !out || ino == 0)
        return EFS_ERR_INVAL;
    for (tries = 1; tries <= EFS_META_STAT_TRIES; tries++) {
        struct efs_meta_row row, again;
        struct efs_txn_pending pend;
        uint64_t size, mtime, ctime, bits;
        uint32_t i, nl = 0;
        int moved = 0, stable = 1, rc, is_dir;

        rc = efs_meta_apply_get_inode(kv, ino, &row);
        if (rc != EFS_OK)
            return rc;
        is_dir = S_ISDIR(row.mode) ? 1 : 0;
        size = row.base_size;
        mtime = row.base_mtime;
        ctime = row.base_ctime;
        /* Files reduce over write lanes; a spread directory reduces over
         * dir lanes (used_shards). A LOCAL directory's times already live
         * on the row, so it is a one-read collect. */
        bits = is_dir
                   ? (row.layout != EFS_META_LAYOUT_LOCAL ? row.used_shards : 0)
                   : row.active_lanes;
        for (i = 0; i < EFS_META_LANES; i++)
            if (bits & (1ULL << i))
                lanes[nl++] = (uint8_t)i;

        memset(&pend, 0, sizeof(pend));
        for (i = 0; i < nl; i++) {
            uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
            uint32_t kl = 0, vn;
            struct efs_txn_reduce red;
            struct lane_rec ln;

            rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lanes[i]), ino,
                                 row.generation, lanes[i], key, &kl);
            if (rc != EFS_OK)
                return rc;
            /* Committed-but-not-yet-materialized reductions have to count:
             * a transaction is visible at its decision, not when a reducer
             * gets around to folding it in, so a stat that ignored them
             * could report a size older than a write that already
             * returned. */
            rc = efs_txn_reduce_read_ex(kv, key, kl, coord, ctx, &red, &pend);
            if (rc != EFS_OK)
                return rc;
            vn = sizeof(val);
            rc = efs_kv_get(kv, key, kl, val, &vn);
            if (rc == EFS_OK) {
                rc = unpack_lane(val, vn, &ln);
                if (rc != EFS_OK)
                    return rc;
            } else if (rc == EFS_ERR_NOT_FOUND) {
                memset(&ln, 0, sizeof(ln));
            } else {
                return rc;
            }
            seq1[i] = ln.seq;
            if (!is_dir)
                size = max_u64(size, red.max_end);
            ctime = max_u64(ctime, red.max_ctime);
            /* A lane mtime stamped before the current utimens generation was
             * invalidated by it, and taking a MAX against it would let the
             * old value win — which is the whole reason utimens has a
             * generation and the other time sources do not.
             *
             * The fence zeros materialized max_mtime and bumps the lane's
             * generation. A committed-but-unmaterialized reduction from
             * before the fence still sits under the lane prefix with the
             * old mtime; folding it in would undo the utimens. After the
             * fence the lane is (gen>0, max_mtime==0); a later write puts
             * a real stamp back and pending reductions count again. */
            if (ln.mtime_gen >= row.mtime_gen) {
                if (!(ln.max_mtime == 0 && ln.mtime_gen > 0))
                    mtime = max_u64(mtime, red.max_mtime);
            }
        }

        /* Second collect: the sequence numbers only. */
        for (i = 0; i < nl && stable; i++) {
            rc = lane_seq_get(kv, ino, row.generation, lanes[i], &seq2[i]);
            if (rc != EFS_OK)
                return rc;
            if (seq2[i] != seq1[i])
                stable = 0;
        }
        rc = efs_txn_pending_recheck(&pend, coord, ctx, &moved);
        if (rc != EFS_OK)
            return rc;
        if (moved)
            stable = 0;
        rc = efs_meta_apply_get_inode(kv, ino, &again);
        if (rc != EFS_OK)
            return rc;
        if (again.content_epoch != row.content_epoch ||
            again.mtime_gen != row.mtime_gen ||
            again.active_lanes != row.active_lanes ||
            again.used_shards != row.used_shards ||
            again.generation != row.generation)
            stable = 0;
        if (!stable)
            continue;

        memset(out, 0, sizeof(*out));
        out->ino = row.ino;
        out->generation = row.generation;
        out->mode = row.mode;
        out->nlink = row.nlink;
        out->uid = row.uid;
        out->gid = row.gid;
        out->size = size;
        out->mtime = mtime;
        out->ctime = ctime;
        out->atime = row.base_atime; /* noatime: reads do not move it */
        out->lanes = nl;
        out->attempts = tries;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

/* Push a per-lane update across a bitmap. Truncate uses this for the epoch
 * fence; utimens uses it for mtime_gen. A bit with no lane record yet is
 * skipped — the first write to that lane will observe the row and stamp
 * itself. `upd` returns 0 to leave the record untouched (replay). */
static int fence_lane_bits(struct efs_kv *kv, efs_ino_t ino, uint64_t generation,
                           uint64_t bits,
                           int (*upd)(struct lane_rec *ln, void *arg),
                           void *arg, struct efs_kv_item *it, uint32_t *n,
                           uint8_t k_ln[][EFS_KV_KEY_MAX],
                           uint8_t v_ln[][LANE_VAL])
{
    uint32_t i;
    int rc;

    for (i = 0; i < EFS_META_LANES; i++) {
        uint8_t old_ln[LANE_VAL];
        uint32_t kl = 0, vn;
        struct lane_rec ln;

        if ((bits & (1ULL << i)) == 0)
            continue;
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, (uint8_t)i), ino, generation,
                             (uint8_t)i, k_ln[i], &kl);
        if (rc != EFS_OK)
            return rc;
        vn = sizeof(old_ln);
        rc = efs_kv_get(kv, k_ln[i], kl, old_ln, &vn);
        if (rc == EFS_ERR_NOT_FOUND)
            continue;
        if (rc != EFS_OK)
            return rc;
        rc = unpack_lane(old_ln, vn, &ln);
        if (rc != EFS_OK)
            return rc;
        if (!upd(&ln, arg))
            continue;
        pack_lane(v_ln[i], &ln);
        it[*n].op = EFS_KV_PUT;
        it[*n].key = k_ln[i];
        it[*n].klen = kl;
        it[*n].val = v_ln[i];
        it[*n].vlen = LANE_VAL;
        (*n)++;
    }
    return EFS_OK;
}

static int epoch_lane_upd(struct lane_rec *ln, void *arg)
{
    ln->fenced_epoch = *(const uint64_t *)arg;
    /* Invalidate the size claim: after a truncate the authoritative size is
     * base_size on the inode row, and a lane still reporting the pre-truncate
     * high-water mark would win the MAX and undo the truncate. Times are not
     * invalidated — a truncate is a modification, so they only move forward. */
    ln->max_end = 0;
    ln->seq++; /* a stat collect in flight must not validate across this */
    return 1;
}

struct trunc_scan {
    struct efs_kv *kv;
    efs_ino_t ino;
    uint64_t gen;
    uint64_t size;
    uint32_t tail_ci;
    uint8_t has_tail;
    struct efs_kv_item *it;
    uint8_t (*del_keys)[EFS_KV_KEY_MAX];
    uint32_t n;
    uint32_t del_base;
    uint32_t cap;
    int rc;
};

static int trunc_del_cb(void *user, const uint8_t *key, uint32_t klen,
                        const uint8_t *val, uint32_t vlen)
{
    struct trunc_scan *ts = user;
    efs_ino_t ino;
    uint64_t gen;
    uint32_t ci;

    (void)val;
    (void)vlen;
    if (klen < 24 || key[2] != EFS_KV_KIND_CHUNK)
        return 0;
    ino = rd64(key + 3);
    gen = rd64(key + 11);
    ci = rd32(key + 20);
    if (ino != ts->ino || gen != ts->gen)
        return 0;
    if (ts->has_tail && ci == ts->tail_ci)
        return 0;
    if (ts->size == 0 ||
        (uint64_t)ci * (uint64_t)EFS_MIN_CHUNK_SIZE >= ts->size)
        goto del;
    return 0;
del:
    if (ts->n >= ts->cap) {
        ts->rc = EFS_ERR_NOMEM;
        return 1;
    }
    memcpy(ts->del_keys[ts->n - ts->del_base], key, klen);
    ts->it[ts->n].op = EFS_KV_DEL;
    ts->it[ts->n].key = ts->del_keys[ts->n - ts->del_base];
    ts->it[ts->n].klen = klen;
    ts->n++;
    return 0;
}

static int truncate_lane_range_del(struct efs_kv *kv, efs_ino_t ino,
                                   uint64_t gen, uint8_t lane, uint64_t size,
                                   uint32_t tail_ci, uint8_t has_tail,
                                   struct efs_kv_item *it,
                                   uint8_t (*del_keys)[EFS_KV_KEY_MAX],
                                   uint32_t *n, uint32_t cap)
{
    uint8_t pref[EFS_KV_KEY_MAX];
    uint32_t plen = 0;
    struct trunc_scan ts;
    int rc;

    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, gen, lane, 0,
                          pref, &plen);
    if (rc != EFS_OK)
        return rc;
    plen = 20;
    memset(&ts, 0, sizeof(ts));
    ts.kv = kv;
    ts.ino = ino;
    ts.gen = gen;
    ts.size = size;
    ts.tail_ci = tail_ci;
    ts.has_tail = has_tail;
    ts.it = it;
    ts.del_keys = del_keys;
    ts.n = *n;
    ts.del_base = *n;
    ts.cap = cap;
    rc = efs_kv_scan_prefix(kv, pref, plen, trunc_del_cb, &ts);
    if (ts.rc != EFS_OK)
        return ts.rc;
    *n = ts.n;
    return rc;
}

static int truncate_publish_tail(struct efs_kv *kv, struct efs_meta_row *row,
                                 uint64_t now, const struct efs_meta_pub *tail,
                                 struct efs_kv_item *it, uint32_t *n,
                                 uint32_t cap, uint8_t *k_ch, uint8_t *v_ch,
                                 uint8_t k_ln[][EFS_KV_KEY_MAX],
                                 uint8_t v_ln[][LANE_VAL])
{
    struct efs_meta_pub p;
    struct efs_meta_chunk got;
    uint8_t lane;
    uint32_t lsh;
    uint8_t old_ch[CHUNK_VAL];
    uint32_t kc = 0, kl = 0, vn;
    uint64_t committed = 0;
    struct lane_rec ln;
    struct efs_meta_chunk stored;
    int rc, have_ln = 0;

    if (!tail)
        return EFS_OK;
    p = *tail;
    p.ino = row->ino;
    p.new_size = tail->new_size;
    p.now = now;
    p.content_epoch = row->content_epoch;
    rc = evidence_ok(&p);
    if (rc != EFS_OK)
        return rc;
    lane = (uint8_t)(p.chunk_index % EFS_META_LANES);
    lsh = efs_kv_lane_shard(row->ino, lane);
    rc = efs_kv_key_chunk(lsh, row->ino, row->generation, lane, p.chunk_index,
                          k_ch, &kc);
    if (rc == EFS_OK)
        rc = efs_kv_key_lane(lsh, row->ino, row->generation, lane, k_ln[lane],
                             &kl);
    if (rc != EFS_OK)
        return rc;
    vn = sizeof(old_ch);
    rc = efs_kv_get(kv, k_ch, kc, old_ch, &vn);
    if (rc == EFS_OK) {
        rc = unpack_chunk(old_ch, vn, &got);
        if (rc != EFS_OK)
            return rc;
        committed = got.generation;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    if (committed == p.candidate_gen)
        return EFS_OK;
    if (p.expected_gen != committed)
        return EFS_ERR_STALE;
    if (unpack_lane(v_ln[lane], LANE_VAL, &ln) == EFS_OK)
        have_ln = 1;
    if (!have_ln) {
        uint8_t old_ln[LANE_VAL];

        vn = sizeof(old_ln);
        rc = efs_kv_get(kv, k_ln[lane], kl, old_ln, &vn);
        if (rc == EFS_OK) {
            rc = unpack_lane(old_ln, vn, &ln);
            if (rc != EFS_OK)
                return rc;
            have_ln = 1;
        } else if (rc != EFS_ERR_NOT_FOUND) {
            return rc;
        }
    }
    if (!have_ln)
        memset(&ln, 0, sizeof(ln));
    if (p.content_epoch < ln.fenced_epoch)
        return EFS_ERR_STALE;
    ln.max_end = max_u64(ln.max_end, p.new_size);
    if (ln.mtime_gen < row->mtime_gen) {
        ln.max_mtime = p.now;
        ln.mtime_gen = row->mtime_gen;
    } else {
        ln.max_mtime = max_u64(ln.max_mtime, p.now);
    }
    ln.max_ctime = max_u64(ln.max_ctime, p.now);
    ln.seq++;
    stored = p.ch;
    stored.generation = p.candidate_gen;
    stored.coding_profile_id = p.coding_profile_id;
    stored.content_epoch = p.content_epoch;
    pack_chunk(v_ch, &stored);
    pack_lane(v_ln[lane], &ln);
    if (*n + 2 > cap)
        return EFS_ERR_NOMEM;
    it[*n].op = EFS_KV_PUT;
    it[*n].key = k_ch;
    it[*n].klen = kc;
    it[*n].val = v_ch;
    it[*n].vlen = CHUNK_VAL;
    (*n)++;
    {
        uint32_t i;
        int found = 0;

        for (i = 0; i < *n; i++) {
            if (it[i].op == EFS_KV_PUT && it[i].key == k_ln[lane]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            it[*n].op = EFS_KV_PUT;
            it[*n].key = k_ln[lane];
            it[*n].klen = kl;
            it[*n].val = v_ln[lane];
            it[*n].vlen = LANE_VAL;
            (*n)++;
        }
    }
    if ((row->active_lanes & (1ULL << lane)) == 0)
        row->active_lanes |= 1ULL << lane;
    return EFS_OK;
}

int efs_meta_apply_truncate(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                            const struct efs_meta_truncate *t)
{
    struct efs_meta_row row;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint8_t v_ino[INO_VAL];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    uint8_t del_keys[EFS_META_LANES * 32][EFS_KV_KEY_MAX];
    uint8_t k_tail[EFS_KV_KEY_MAX], v_tail[CHUNK_VAL];
    struct efs_kv_item it[EFS_META_LANES + 1 + EFS_META_LANES * 32 + 2];
    uint32_t ki = 0, n = 0, i, tail_ci = 0;
    uint64_t new_epoch;
    uint8_t has_tail = 0;
    int touch_inode = 0;
    int rc;

    if (!kv || !t || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISREG(row.mode))
        return EFS_ERR_INVAL;
    if (t->expect_gen != 0 && t->expect_gen != row.generation)
        return EFS_ERR_STALE;
    if (t->size > 0 && (t->size % EFS_MIN_CHUNK_SIZE) != 0) {
        tail_ci = (uint32_t)(t->size / EFS_MIN_CHUNK_SIZE);
        has_tail = t->tail != NULL;
        if (!has_tail)
            return EFS_ERR_INVAL;
        if (t->tail->chunk_index != tail_ci)
            return EFS_ERR_INVAL;
    }
    new_epoch = row.content_epoch + 1;
    row.content_epoch = new_epoch;
    row.base_size = t->size;
    row.base_mtime = max_u64(row.base_mtime, now);
    row.base_ctime = max_u64(row.base_ctime, now);
    memset(it, 0, sizeof(it));
    memset(k_ln, 0, sizeof(k_ln));
    memset(v_ln, 0, sizeof(v_ln));
    for (i = 0; i < EFS_META_LANES; i++) {
        if ((row.active_lanes & (1ULL << i)) == 0)
            continue;
        rc = fence_lane_bits(kv, ino, row.generation, 1ULL << i, epoch_lane_upd,
                             &new_epoch, it, &n, k_ln, v_ln);
        if (rc != EFS_OK)
            return rc;
        rc = truncate_lane_range_del(kv, ino, row.generation, (uint8_t)i,
                                   t->size, tail_ci, has_tail, it, del_keys,
                                   &n, (uint32_t)(sizeof(it) / sizeof(it[0])));
        if (rc != EFS_OK)
            return rc;
    }
    rc = truncate_publish_tail(kv, &row, now, t->tail, it, &n,
                               (uint32_t)(sizeof(it) / sizeof(it[0])),
                               k_tail, v_tail, k_ln, v_ln);
    if (rc != EFS_OK)
        return rc;
    if (t->tail)
        touch_inode = 1;
    pack_inode(v_ino, &row);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    (void)touch_inode;
    return efs_kv_batch(kv, it, n);
}

static int utimens_lane_upd(struct lane_rec *ln, void *arg)
{
    uint64_t gen = *(const uint64_t *)arg;

    if (ln->mtime_gen >= gen)
        return 0;
    ln->mtime_gen = gen;
    /* Zero, not the utimens value: getattr takes base_mtime, and a later
     * write MAX(0, now) must not resurrect the pre-utimens stamp. */
    ln->max_mtime = 0;
    ln->seq++;
    return 1;
}

int efs_meta_apply_epoch_fence(struct efs_kv *kv, efs_ino_t ino)
{
    struct efs_meta_row row;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint8_t v_ino[INO_VAL];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    struct efs_kv_item it[EFS_META_LANES + 1];
    uint32_t ki = 0, n = 0;
    int rc;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    row.content_epoch++;
    pack_inode(v_ino, &row);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    /* Every active lane, not just lane 0: a lane the fence skipped keeps
     * believing its own epoch and its own size. */
    rc = fence_lane_bits(kv, ino, row.generation, row.active_lanes,
                         epoch_lane_upd, &row.content_epoch, it, &n, k_ln, v_ln);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_batch(kv, it, n);
}

int efs_meta_apply_utimens(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                           const struct efs_meta_utimens *u)
{
    struct efs_meta_row row;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint8_t v_ino[INO_VAL];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    struct efs_kv_item it[EFS_META_LANES + 1];
    uint32_t ki = 0, n = 0;
    uint64_t bits;
    int rc;

    if (!kv || !u || ino == 0)
        return EFS_ERR_INVAL;
    if (u->mask == 0 ||
        (u->mask & ~(EFS_META_SET_MTIME | EFS_META_SET_ATIME)) != 0)
        return EFS_ERR_INVAL;
    if ((u->mask & EFS_META_SET_MTIME) && u->mtime_gen == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (u->expect_gen != 0 && u->expect_gen != row.generation)
        return EFS_ERR_STALE;
    if ((u->mask & EFS_META_SET_MTIME) && u->mtime_gen < row.mtime_gen)
        return EFS_ERR_STALE;
    if (u->mask & EFS_META_SET_ATIME)
        row.base_atime = u->atime;
    if (u->mask & EFS_META_SET_MTIME)
        row.base_mtime = u->mtime;
    /* ctime always moves (POSIX), MAX-clamped like every implicit stamp. */
    row.base_ctime = max_u64(row.base_ctime, now);
    if (u->mask & EFS_META_SET_MTIME)
        row.mtime_gen = u->mtime_gen;
    pack_inode(v_ino, &row);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    if (u->mask & EFS_META_SET_MTIME) {
        bits = S_ISDIR(row.mode) && row.layout != EFS_META_LAYOUT_LOCAL
                   ? row.used_shards
                   : row.active_lanes;
        rc = fence_lane_bits(kv, ino, row.generation, bits, utimens_lane_upd,
                             &row.mtime_gen, it, &n, k_ln, v_ln);
        if (rc != EFS_OK)
            return rc;
    }
    return efs_kv_batch(kv, it, n);
}

int efs_meta_apply_get_chunk(struct efs_kv *kv, efs_ino_t ino, uint32_t chunk_index,
                             struct efs_meta_chunk *out)
{
    struct efs_meta_row row;
    uint8_t lane;
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    if (!kv || !out || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    lane = (uint8_t)(chunk_index % EFS_META_LANES);
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, row.generation, lane,
                          chunk_index, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    return unpack_chunk(val, vlen, out);
}

struct check_scan {
    struct efs_kv *kv;
    int rc;
    int saw_root;
};

static int check_cb(void *user, const uint8_t *key, uint32_t klen,
                    const uint8_t *val, uint32_t vlen)
{
    struct check_scan *c = user;
    struct efs_meta_dentry d;
    struct efs_meta_row r;
    int rc;

    if (klen < 3)
        return 0;
    if (key[2] == EFS_KV_KIND_INODE) {
        rc = unpack_inode(val, vlen, &r);
        if (rc != EFS_OK) {
            c->rc = rc;
            return 1;
        }
        if (r.ino == EFS_ROOT_INO)
            c->saw_root = 1;
        if (efs_kv_inode_shard(r.ino) != (((uint32_t)key[0] << 8) | key[1])) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        return 0;
    }
    if (key[2] != EFS_KV_KIND_DENTRY)
        return 0;
    rc = unpack_dentry(val, vlen, &d);
    if (rc != EFS_OK) {
        c->rc = rc;
        return 1;
    }
    if (d.type == EFS_META_DENT_TOMBSTONE || d.ino == 0)
        return 0;
    rc = efs_meta_apply_get_inode(c->kv, d.ino, &r);
    if (rc == EFS_ERR_NOT_FOUND) {
        c->rc = EFS_ERR_IO; /* I9 */
        return 1;
    }
    if (rc != EFS_OK) {
        c->rc = rc;
        return 1;
    }
    return 0;
}

int efs_meta_apply_check(struct efs_kv *kv)
{
    struct check_scan c = { .kv = kv, .rc = EFS_OK, .saw_root = 0 };
    int rc;

    if (!kv)
        return EFS_ERR_INVAL;
    rc = efs_kv_scan(kv, check_cb, &c);
    if (c.rc != EFS_OK)
        return c.rc;
    if (rc != EFS_OK)
        return rc;
    if (!c.saw_root)
        return EFS_ERR_PROTO;
    return EFS_OK;
}
