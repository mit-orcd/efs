#include "efs/meta_apply.h"
#include "efs/kv_key.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define INO_VAL  80
#define DENT_VAL 20
#define ALLOC_VAL 8
#define LANE_VAL 32
#define CHUNK_VAL (4 * EFS_NUM_FRAGMENTS + EFS_HASH_SIZE * EFS_NUM_FRAGMENTS)

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

static void pack_inode(uint8_t *p, const struct efs_meta_row *r)
{
    be64(p + 0, r->ino);
    be64(p + 8, r->generation);
    be32(p + 16, r->mode);
    be32(p + 20, r->nlink);
    be64(p + 24, r->parent);
    be64(p + 32, r->base_size);
    be64(p + 40, r->active_lanes);
    be64(p + 48, 0); /* content_epoch */
    be64(p + 56, 0); /* mtime_gen */
    be64(p + 64, 0); /* base_mtime */
    be64(p + 72, 0); /* base_ctime */
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

int efs_meta_apply_lookup(struct efs_kv *kv, efs_ino_t parent, const char *name,
                          struct efs_meta_dentry *out)
{
    uint8_t key[EFS_KV_KEY_MAX], val[DENT_VAL];
    uint32_t klen = 0, vlen;
    uint32_t shard;
    int rc;

    if (!kv || !name || !out || parent == 0)
        return EFS_ERR_INVAL;
    shard = efs_kv_inode_shard(parent);
    rc = efs_kv_key_dentry(shard, parent, name, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    return unpack_dentry(val, vlen, out);
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

int efs_meta_apply_init(struct efs_kv *kv)
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

int efs_meta_apply_peek_alloc(struct efs_kv *kv, uint32_t shard, efs_ino_t *next)
{
    if (!kv || !next)
        return EFS_ERR_INVAL;
    return load_alloc(kv, shard, next);
}

static int create_file_batch(struct efs_kv *kv, efs_ino_t parent, uint32_t mode,
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
    struct efs_kv_item it[4];
    uint32_t n = 0, shard;
    efs_ino_t next = 0, ino;
    int rc;

    if (!kv || !name || parent == 0)
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

    shard = efs_kv_inode_shard(parent);
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

int efs_meta_apply_create_file(struct efs_kv *kv, efs_ino_t parent, uint32_t mode,
                               const char *name, efs_ino_t *out)
{
    return create_file_batch(kv, parent, mode, name, NULL, out);
}

int efs_meta_apply_create_file_op(struct efs_kv *kv, const struct efs_opid *op,
                                  efs_ino_t parent, uint32_t mode, const char *name,
                                  efs_ino_t *out)
{
    if (!op)
        return EFS_ERR_INVAL;
    return create_file_batch(kv, parent, mode, name, op, out);
}

int efs_meta_apply_unlink(struct efs_kv *kv, efs_ino_t parent, const char *name)
{
    struct efs_meta_dentry dent;
    struct efs_meta_row row;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint32_t kd = 0, ki = 0, shard;
    struct efs_kv_item it[2];
    uint32_t n = 0;
    int rc;

    rc = efs_meta_apply_resolve(kv, parent, name, &dent, &row);
    if (rc != EFS_OK)
        return rc;
    if (S_ISDIR(row.mode))
        return EFS_ERR_INVAL;
    shard = efs_kv_inode_shard(parent);
    rc = efs_kv_key_dentry(shard, parent, name, k_dent, &kd);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_DEL;
    it[n].key = k_dent;
    it[n].klen = kd;
    n++;
    if (row.nlink <= 1) {
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
        it[n].op = EFS_KV_DEL;
        it[n].key = k_ino;
        it[n].klen = ki;
        n++;
    }
    return efs_kv_batch(kv, it, n);
}

static void pack_chunk(uint8_t *p, const struct efs_meta_chunk *ch)
{
    int i;

    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        be32(p + (uint32_t)i * 4u, ch->nodes[i]);
    memcpy(p + 4 * EFS_NUM_FRAGMENTS, ch->checksums,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
}

static int unpack_chunk(const uint8_t *p, uint32_t n, struct efs_meta_chunk *ch)
{
    int i;

    if (!p || !ch || n < CHUNK_VAL)
        return EFS_ERR_PROTO;
    memset(ch, 0, sizeof(*ch));
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        ch->nodes[i] = rd32(p + (uint32_t)i * 4u);
    memcpy(ch->checksums, p + 4 * EFS_NUM_FRAGMENTS,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
    return EFS_OK;
}

int efs_meta_apply_publish(struct efs_kv *kv, efs_ino_t ino, uint32_t chunk_index,
                           uint64_t new_size, const struct efs_meta_chunk *ch)
{
    struct efs_meta_row row;
    uint8_t lane;
    uint32_t lsh, ish;
    uint8_t k_ch[EFS_KV_KEY_MAX], k_ln[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t v_ch[CHUNK_VAL], v_ln[LANE_VAL], v_ino[INO_VAL];
    uint8_t old_ln[LANE_VAL];
    uint32_t kc = 0, kl = 0, ki = 0, vn;
    uint64_t sz, mt = 0, ct = 0, seq = 1;
    struct efs_kv_item it[3];
    uint32_t n = 0;
    int rc;

    if (!kv || !ch || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    lane = (uint8_t)(chunk_index % EFS_META_LANES);
    lsh = efs_kv_lane_shard(ino, lane);
    ish = efs_kv_inode_shard(ino);
    rc = efs_kv_key_chunk(lsh, ino, row.generation, lane, chunk_index, k_ch, &kc);
    if (rc == EFS_OK)
        rc = efs_kv_key_lane(lsh, ino, row.generation, lane, k_ln, &kl);
    if (rc != EFS_OK)
        return rc;
    pack_chunk(v_ch, ch);
    vn = sizeof(old_ln);
    rc = efs_kv_get(kv, k_ln, kl, old_ln, &vn);
    if (rc == EFS_OK && vn >= LANE_VAL) {
        uint64_t old = rd64(old_ln);
        sz = old > new_size ? old : new_size;
        mt = rd64(old_ln + 8);
        ct = rd64(old_ln + 16);
        seq = rd64(old_ln + 24) + 1;
    } else if (rc == EFS_ERR_NOT_FOUND) {
        sz = new_size;
    } else if (rc != EFS_OK) {
        return rc;
    } else {
        sz = new_size;
    }
    be64(v_ln + 0, sz);
    be64(v_ln + 8, mt);
    be64(v_ln + 16, ct);
    be64(v_ln + 24, seq);

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
    if (lsh == ish) {
        row.active_lanes |= 1ULL << lane;
        if (new_size > row.base_size)
            row.base_size = new_size;
        pack_inode(v_ino, &row);
        rc = efs_kv_key_inode(ish, ino, k_ino, &ki);
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
