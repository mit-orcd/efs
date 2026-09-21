#include "efs/meta_apply.h"
#include "efs/raft.h"
#include "efs/kv_key.h"
#include "efs/session.h"
#include "efs/checksum.h"
#include "efs/dir_layout.h"
#include "efs/dir_spread.h"
#include "efs/txn.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define INO_VAL  EFS_META_INO_BYTES
#define DENT_VAL 20
#define ALLOC_VAL 8
#define LANE_VAL 56
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
    uint64_t append_bar; /* live reservation watermark; 0 = no barrier */
};

static void pack_lane(uint8_t *p, const struct lane_rec *l)
{
    be64(p + 0, l->max_end);
    be64(p + 8, l->max_mtime);
    be64(p + 16, l->max_ctime);
    be64(p + 24, l->seq);
    be64(p + 32, l->fenced_epoch);
    be64(p + 40, l->mtime_gen);
    be64(p + 48, l->append_bar);
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
    if (n >= 56)
        l->append_bar = rd64(p + 48);
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

int efs_meta_stamp_dir_lane(struct efs_kv *kv, const struct efs_meta_row *dir,
                            const char *name, uint64_t now, uint8_t *key,
                            uint32_t *klen, uint8_t *val, uint32_t cap)
{
    if (!kv || !dir || !name || !key || !klen || !val || cap < LANE_VAL)
        return EFS_ERR_INVAL;
    return dir_lane_stamp(kv, dir, name, now, key, klen, val);
}

/* Per-(dir, dentry-shard) emptiness witness. Every insert (and every other
 * mutation of that shard's names) bumps it so an RMDIR that observed empty
 * cannot commit across a concurrent create (directory.md). */
static int dseq_bump(struct efs_kv *kv, uint32_t shard, efs_ino_t dir,
                     uint8_t lane, uint8_t *k, uint32_t *kl, uint8_t *v)
{
    uint8_t buf[8];
    uint32_t n = 8;
    uint64_t seq = 0;
    int rc;

    rc = efs_kv_key_dseq(shard, dir, lane, k, kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, k, *kl, buf, &n);
    if (rc == EFS_OK && n >= 8)
        seq = rd64(buf);
    else if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND)
        return rc;
    be64(v, seq + 1);
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
    memset(p + 57, 0, 3);
    be32(p + 60, r->nents);
    be64(p + 64, r->layout_epoch);
    be64(p + 72, r->used_shards);
    be32(p + 80, r->uid);
    be32(p + 84, r->gid);
    be64(p + 88, r->base_mtime);
    be64(p + 96, r->base_atime);
    be64(p + 104, r->base_ctime);
    be64(p + 112, r->mtime_gen);
    be64(p + 120, r->parent_version);
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
    r->nents = rd32(p + 60);
    r->layout_epoch = rd64(p + 64);
    r->used_shards = rd64(p + 72);
    r->uid = rd32(p + 80);
    r->gid = rd32(p + 84);
    r->base_mtime = rd64(p + 88);
    r->base_atime = rd64(p + 96);
    r->base_ctime = rd64(p + 104);
    r->mtime_gen = rd64(p + 112);
    r->parent_version = rd64(p + 120);
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
    if (rc == EFS_ERR_NOT_FOUND) {
        /* Parent row lives on another Raft group. Try hashed then local. */
        hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
        rc = dent_get(kv, hsh, parent, name, out);
        if (rc == EFS_OK) {
            if (out->type == EFS_META_DENT_TOMBSTONE)
                return EFS_ERR_NOT_FOUND;
            return EFS_OK;
        }
        if (rc != EFS_ERR_NOT_FOUND)
            return rc;
        return dent_get(kv, efs_kv_inode_shard(parent), parent, name, out);
    }
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

/* Watermark can sit on a live row (SNAP lag, hollow replica, or a
 * rewind after unlink of a later sibling). Reusing that ino overwrites
 * the row and inherits leftover children — POSIX mkdir then EEXIST on
 * well-known names like p/c. Skip occupied inodes. */
#define ALLOC_SKIP_MAX 4096

static int alloc_next_free(struct efs_kv *kv, uint32_t shard, efs_ino_t *ino,
                           efs_ino_t *next_out)
{
    struct efs_meta_row row;
    efs_ino_t next;
    int rc, n;

    rc = load_alloc(kv, shard, &next);
    if (rc != EFS_OK)
        return rc;
    for (n = 0; n < ALLOC_SKIP_MAX; n++) {
        if (efs_kv_inode_shard(next) != shard) {
            next = shard ? (efs_ino_t)shard
                         : (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
            if (next == EFS_ROOT_INO)
                next += (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
        }
        rc = efs_meta_apply_get_inode(kv, next, &row);
        if (rc == EFS_ERR_NOT_FOUND) {
            *ino = next;
            *next_out = next + (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
            return EFS_OK;
        }
        if (rc != EFS_OK)
            return rc;
        next += (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
    }
    return EFS_ERR_NOMEM;
}

/* Log-path allocation with a proposer-chosen HINT. The host picks `want`
 * on the proposing node from its APPLIED alloc watermark
 * (efs_meta_apply_peek_alloc) and packs it into the CREATE command; two
 * creates on one shard that peek before either applies (different voters,
 * or a forwarded create racing the leader's own) carry the SAME ino. The
 * old apply took `want` unconditionally, so the second PUT overwrote the
 * first file's inode row under the same key: two names → one row, both
 * writers publishing onto one ino (IO-500 ior-easy: 36 files, 10 distinct
 * inos, 76108 read-verify errors), and the first unlink left the other
 * name dangling NOT_FOUND. The apply is the allocator: honor the hint only
 * if it is still unallocated (>= watermark AND no row — the row check
 * covers a SNAP-lag rewound watermark), else take the next free ino.
 * Deterministic on every replica of the group (same log, same shard
 * state). Callers read the ino back from the dentry, never from the hint. */
static int alloc_hint_or_next(struct efs_kv *kv, uint32_t shard,
                              efs_ino_t want, efs_ino_t *ino,
                              efs_ino_t *next_out)
{
    struct efs_meta_row row;
    efs_ino_t next;
    int rc;

    if (efs_kv_inode_shard(want) != shard)
        return EFS_ERR_PROTO;
    rc = load_alloc(kv, shard, &next);
    if (rc != EFS_OK)
        return rc;
    if (want >= next) {
        rc = efs_meta_apply_get_inode(kv, want, &row);
        if (rc == EFS_ERR_NOT_FOUND) {
            *ino = want;
            *next_out = want + (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
            return EFS_OK;
        }
        if (rc != EFS_OK)
            return rc;
    }
    return alloc_next_free(kv, shard, ino, next_out);
}

/* The per-shard ALLOC key is written by two paths: this log path (file
 * CREATE, same-group MKDIR) and the host's txn path (cross-group MKDIR,
 * hashed CREATE), which PREPAREs an EXCL intent on it at the version it
 * read and PUTs the child row + new watermark at resolve. The txn treats
 * the intent as its lock and the version as its CAS; the log path honored
 * neither, so the two allocators raced in both orders: (1) intent pending →
 * the log-path create took the same ino and the txn's resolve overwrote the
 * row; (2) log-path alloc write unversioned → a txn that peeked BEFORE it
 * still PREPAREd at the old version and won. posix names_* : file `cafeé`
 * (parent shard 650) and dir `aaaa…` (mkdir_shard 650) both got ino 4746,
 * the dir's unlink deleted the row, the file dangled (the old
 * `cwi-fail: ino_dup` family). Rule now: a log-path alloc under a pending
 * intent is BUSY (the txn rule — "PREPARE is no-wait, conflict → BUSY";
 * the client retries), and every log-path alloc write bumps the key's
 * version in the same batch so an outdated PREPARE is STALE, exactly as a
 * resolve would. Deterministic: intents and versions are shard state of
 * the same group's log. */
static int alloc_key_claim(struct efs_kv *kv, const uint8_t *k_alloc,
                           uint32_t ka, uint8_t *k_ver, uint32_t *kver,
                           uint8_t v_ver[8])
{
    uint8_t ik[EFS_KV_KEY_MAX], buf[8];
    uint32_t il = 0, n = 0;
    uint64_t ver = 0;
    int rc;

    rc = efs_kv_key_intent(k_alloc, ka, ik, &il);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, ik, il, buf, &n); /* probe: found → INVAL */
    if (rc == EFS_OK || rc == EFS_ERR_INVAL)
        return EFS_ERR_BUSY;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    *kver = 0;
    rc = efs_kv_key_ver(k_alloc, ka, k_ver, kver);
    if (rc != EFS_OK)
        return rc;
    n = 8;
    rc = efs_kv_get(kv, k_ver, *kver, buf, &n);
    if (rc == EFS_OK && n >= 8)
        ver = rd64(buf);
    else if (rc != EFS_ERR_NOT_FOUND && rc != EFS_OK)
        return rc;
    be64(v_ver, ver + 1);
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

/* Production host packs a zero UUID / seq 0 until sessions are hosted.
 * That must not PUT the op-id window (zero UUID hashes off the inode
 * group) and must not call lookup (seq 0 is an error). */
static int opid_hosted(const struct efs_opid *op)
{
    uint32_t i;

    if (!op || op->seq == 0)
        return 0;
    for (i = 0; i < EFS_OPID_UUID_LEN; i++) {
        if (op->client_uuid[i])
            return 1;
    }
    return 0;
}

int efs_meta_apply_mkfs(struct efs_kv *kv, uint64_t now, uint64_t salt)
{
    struct efs_meta_row root;
    uint8_t k_ino[EFS_KV_KEY_MAX], k_alloc[EFS_KV_KEY_MAX];
    uint8_t k_ex[EFS_KV_KEY_MAX], v_ino[INO_VAL], v_alloc[ALLOC_VAL];
    uint8_t v_ex[8];
    uint32_t lk = 0, ak = 0, ek = 0;
    struct efs_kv_item it[3];
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
    be64(v_ex, salt);
    rc = efs_kv_key_inode(shard, EFS_ROOT_INO, k_ino, &lk);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(shard, k_alloc, &ak);
    if (rc == EFS_OK)
        rc = efs_kv_key_export(shard, k_ex, &ek);
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
    it[2].op = EFS_KV_PUT;
    it[2].key = k_ex;
    it[2].klen = ek;
    it[2].val = v_ex;
    it[2].vlen = 8;
    return efs_kv_batch(kv, it, 3);
}

int efs_meta_apply_init(struct efs_kv *kv, uint64_t now)
{
    return efs_meta_apply_mkfs(kv, now, 0);
}

/* The salt record lives on the ROOT shard's export key (written by MKFS,
 * group 0) AND on the even group's anchor shard (written by
 * EFS_MD_CMD_SALT at mkfs time). A node hosting only the even group never
 * applies MKFS, so without the second record its salt read returned 0 and
 * every salted placement (MKDIR scatter) diverged — apply PROTO skips,
 * missing rows, later EIO. Read root anchor first, even anchor second. */
int efs_meta_apply_export_salt(struct efs_kv *kv, uint64_t *out)
{
    uint8_t k[EFS_KV_KEY_MAX], v[8];
    uint32_t kl = 0, vl = 8;
    uint32_t shard = efs_kv_inode_shard(EFS_ROOT_INO);
    int rc;

    if (!kv || !out)
        return EFS_ERR_INVAL;
    *out = 0;
    rc = efs_kv_key_export(shard, k, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, k, kl, v, &vl);
    if (rc == EFS_ERR_NOT_FOUND) {
        uint32_t kl2 = 0, vl2 = 8;

        rc = efs_kv_key_export(efs_kv_anchor_shard(2), k, &kl2);
        if (rc != EFS_OK)
            return rc;
        rc = efs_kv_get(kv, k, kl2, v, &vl2);
        if (rc == EFS_ERR_NOT_FOUND)
            return EFS_ERR_NOT_FOUND; /* no record on either anchor */
        if (rc != EFS_OK)
            return rc;
        vl = vl2;
    } else if (rc != EFS_OK)
        return rc;
    if (vl < 8)
        return EFS_ERR_PROTO;
    *out = rd64(v);
    return EFS_OK;
}

/* EFS_MD_CMD_SALT apply: write the export-salt record on the given anchor
 * shard. Idempotent — an existing record must MATCH (placement is derived
 * from the salt; silently overwriting would fork the namespace). */
int efs_meta_apply_salt_record(struct efs_kv *kv, uint32_t anchor,
                               uint64_t salt)
{
    uint8_t k[EFS_KV_KEY_MAX], v[8];
    uint32_t kl = 0, vl = 8;
    int rc;

    if (!kv)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_export(anchor, k, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, k, kl, v, &vl);
    if (rc == EFS_OK) {
        if (vl >= 8 && rd64(v) == salt)
            return EFS_OK;
        return EFS_ERR_PROTO;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    be64(v, salt);
    return efs_kv_put(kv, k, kl, v, 8);
}

int efs_meta_pack_inode(const struct efs_meta_row *r, uint8_t *out, uint32_t cap)
{
    if (!r || !out || cap < INO_VAL)
        return EFS_ERR_INVAL;
    pack_inode(out, r);
    return EFS_OK;
}

int efs_meta_unpack_inode(const uint8_t *p, uint32_t n, struct efs_meta_row *r)
{
    return unpack_inode(p, n, r);
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
    efs_ino_t ino, nxt;
    int rc;

    if (!kv || !next)
        return EFS_ERR_INVAL;
    rc = alloc_next_free(kv, shard, &ino, &nxt);
    if (rc != EFS_OK)
        return rc;
    *next = ino;
    return EFS_OK;
}

static int create_file_batch(struct efs_kv *kv, const struct efs_meta_attrs *at,
                             efs_ino_t parent, uint32_t mode,
                             const char *name, const struct efs_opid *op,
                             int from_log, efs_ino_t want_ino, int want_layout,
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
    struct efs_kv_item it[9];
    uint32_t n = 0, shard;
    efs_ino_t next = 0, ino;
    uint8_t k_aver[EFS_KV_KEY_MAX], v_aver[8];
    uint32_t kav = 0;
    uint8_t k_par[EFS_KV_KEY_MAX], v_par[INO_VAL];
    uint8_t k_ln[EFS_KV_KEY_MAX], v_ln[LANE_VAL];
    uint8_t k_dseq[EFS_KV_KEY_MAX], v_dseq[8];
    uint32_t kp = 0, kln = 0, ks = 0;
    uint8_t dseq_lane;
    uint64_t bit;
    int rc;
    int touch_parent = 0;
    int stamp_lane = 0;
    int remote_parent = 0;

    if (!kv || !name || !at || parent == 0)
        return EFS_ERR_INVAL;
    if ((mode & S_IFMT) == S_IFDIR)
        return EFS_ERR_INVAL; /* MKDIR is a 2-shard txn; not this helper */
    rc = efs_meta_apply_get_inode(kv, parent, &parent_row);
    if (rc == EFS_ERR_NOT_FOUND) {
        uint8_t pgrp = efs_raft_shard_group(efs_kv_inode_shard(parent));
        uint32_t hdsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
        int hashed = (pgrp != efs_raft_shard_group(hdsh));
        int lay;

        /* A committed log entry must write the child even when this replica
         * lacks the parent row (follower lag / dual-host apply). A helper
         * call still treats same-group missing parent as a hole.
         * Do not guess HASHED from a missing parent on the log path — a
         * dual-host replica that can see a LOCAL parent would write
         * different keys. Packed layout/ino from the leader wins. */
        if (from_log)
            lay = want_layout >= 0 ? want_layout : EFS_META_LAYOUT_LOCAL;
        else if (hashed)
            lay = EFS_META_LAYOUT_HASHED;
        else
            lay = -1;
        if (lay >= 0) {
            memset(&parent_row, 0, sizeof(parent_row));
            parent_row.ino = parent;
            parent_row.generation = 1;
            parent_row.mode = S_IFDIR | 0755;
            parent_row.layout = (uint8_t)lay;
            parent_row.used_shards = (lay == EFS_META_LAYOUT_LOCAL)
                                         ? 0
                                         : ~(uint64_t)0;
            remote_parent = 1;
            rc = EFS_OK;
        }
    }
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

    {
        uint8_t lay = want_layout >= 0 ? (uint8_t)want_layout
                                       : parent_row.layout;

        shard = efs_kv_dentry_shard(parent, name, lay);
        dseq_lane = lay == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    }
    if (want_ino) {
        rc = alloc_hint_or_next(kv, shard, want_ino, &ino, &next);
        if (rc != EFS_OK)
            return rc;
    } else {
        rc = alloc_next_free(kv, shard, &ino, &next);
        if (rc != EFS_OK)
            return rc;
        if (efs_kv_inode_shard(ino) != shard)
            return EFS_ERR_PROTO;
    }

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
    if (rc == EFS_OK)
        rc = alloc_key_claim(kv, k_alloc, ka, k_aver, &kav, v_aver);
    if (rc != EFS_OK)
        return rc;

    /* POSIX: adding an entry moves the containing directory's mtime and
     * ctime. For a LOCAL directory the parent row is on this very shard, so
     * it rides the same atomic batch and costs nothing. For a spread
     * directory it deliberately does NOT go here — routing every create back
     * to the parent's shard is the hotspot the spread exists to remove, so
     * those times live in a per-dentry-shard dir lane (§7.4). */
    if (remote_parent) {
        /* Parent row is on another Raft group. Write child + hashed
         * dentry only — a fabricated parent PUT would pollute this
         * namespace, and a lane stamp needs the real generation. */
    } else if (parent_row.layout == EFS_META_LAYOUT_LOCAL) {
        parent_row.base_mtime = max_u64(parent_row.base_mtime, at->now);
        parent_row.base_ctime = max_u64(parent_row.base_ctime, at->now);
        efs_meta_dir_note_entry(&parent_row, 1);
        touch_parent = 1;
    } else {
        efs_dir_spread_seen(&parent_row);
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
    it[n].op = EFS_KV_PUT;
    it[n].key = k_aver;
    it[n].klen = kav;
    it[n].val = v_aver;
    it[n].vlen = 8;
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
    rc = dseq_bump(kv, shard, parent, dseq_lane, k_dseq, &ks, v_dseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
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

int efs_meta_apply_create_file(struct efs_kv *kv, const struct efs_meta_attrs *at,
                               efs_ino_t parent, uint32_t mode,
                               const char *name, efs_ino_t *out)
{
    return create_file_batch(kv, at, parent, mode, name, NULL, 0, 0, -1, out);
}

int efs_meta_apply_create_file_op(struct efs_kv *kv, const struct efs_opid *op,
                                  const struct efs_meta_attrs *at,
                                  efs_ino_t parent, uint32_t mode, const char *name,
                                  efs_ino_t *out)
{
    if (!op)
        return EFS_ERR_INVAL;
    return create_file_batch(kv, at, parent, mode, name, op, 0, 0, -1, out);
}

int efs_meta_apply_create_file_log(struct efs_kv *kv, const struct efs_meta_attrs *at,
                                   efs_ino_t parent, uint32_t mode,
                                   const char *name, efs_ino_t *out)
{
    return create_file_batch(kv, at, parent, mode, name, NULL, 1, 0, -1, out);
}

int efs_meta_apply_create_file_log_at(struct efs_kv *kv, const struct efs_meta_attrs *at,
                                      efs_ino_t parent, uint32_t mode,
                                      const char *name, efs_ino_t ino,
                                      uint8_t layout, efs_ino_t *out)
{
    if (!ino)
        return EFS_ERR_INVAL;
    return create_file_batch(kv, at, parent, mode, name, NULL, 1, ino,
                             (int)layout, out);
}

static int mkdir_batch(struct efs_kv *kv, const struct efs_meta_attrs *at,
                       efs_ino_t parent, uint32_t mode, const char *name,
                       int from_log, efs_ino_t want_ino, int want_layout,
                       efs_ino_t *out)
{
    struct efs_meta_dentry dent;
    struct efs_meta_row row, parent_row;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX];
    uint8_t k_ln[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[DENT_VAL], v_ino[INO_VAL], v_alloc[ALLOC_VAL];
    uint8_t v_par[INO_VAL], v_ln[LANE_VAL], v_dseq[8];
    uint32_t kd = 0, ki = 0, ka = 0, kp = 0, kln = 0, ks = 0;
    uint8_t k_aver[EFS_KV_KEY_MAX], v_aver[8];
    uint32_t kav = 0;
    struct efs_kv_item it[9];
    uint32_t n = 0, psh, csh, dsh;
    efs_ino_t next = 0, ino;
    uint64_t salt = 0, bit;
    uint8_t dseq_lane, lay;
    int rc;
    int stamp_lane = 0;
    int remote_parent = 0;
    int touch_parent = 0;

    if (!kv || !name || !at || parent == 0 || name[0] == '\0')
        return EFS_ERR_INVAL;
    if ((mode & S_IFMT) != S_IFDIR)
        return EFS_ERR_INVAL;
    if ((mode & 07777) == 0)
        mode |= 0755;
    rc = efs_meta_apply_get_inode(kv, parent, &parent_row);
    if (rc == EFS_ERR_NOT_FOUND && from_log) {
        memset(&parent_row, 0, sizeof(parent_row));
        parent_row.ino = parent;
        parent_row.generation = 1;
        parent_row.mode = S_IFDIR | 0755;
        parent_row.nlink = 2;
        lay = want_layout >= 0 ? (uint8_t)want_layout : EFS_META_LAYOUT_LOCAL;
        parent_row.layout = lay;
        parent_row.used_shards = (lay == EFS_META_LAYOUT_LOCAL)
                                     ? 0
                                     : ~(uint64_t)0;
        remote_parent = 1;
        rc = EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(parent_row.mode))
        return EFS_ERR_INVAL;
    if (parent_row.layout != EFS_META_LAYOUT_LOCAL &&
        parent_row.layout != EFS_META_LAYOUT_HASHED &&
        parent_row.layout != EFS_META_LAYOUT_SPLITTING)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_lookup(kv, parent, name, &dent);
    if (rc == EFS_OK)
        return EFS_ERR_EXIST;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    rc = efs_meta_apply_export_salt(kv, &salt);
    if (rc != EFS_OK)
        return rc;
    lay = want_layout >= 0 ? (uint8_t)want_layout : parent_row.layout;
    psh = efs_kv_inode_shard(parent);
    csh = efs_kv_mkdir_shard(parent, name, salt);
    dsh = efs_kv_dentry_shard(parent, name, lay);
    dseq_lane = lay == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    if (want_ino) {
        rc = alloc_hint_or_next(kv, csh, want_ino, &ino, &next);
        if (rc != EFS_OK)
            return rc;
    } else {
        rc = alloc_next_free(kv, csh, &ino, &next);
        if (rc != EFS_OK)
            return rc;
        if (efs_kv_inode_shard(ino) != csh)
            return EFS_ERR_PROTO;
    }

    memset(&row, 0, sizeof(row));
    row.ino = ino;
    row.generation = 1;
    row.mode = mode;
    row.nlink = 2;
    row.parent = parent;
    row.uid = at->uid;
    row.gid = at->gid;
    row.base_mtime = at->now;
    row.base_atime = at->now;
    row.base_ctime = at->now;
    memset(&dent, 0, sizeof(dent));
    dent.ino = ino;
    dent.generation = 1;
    dent.type = S_IFDIR;
    if (remote_parent) {
        /* Hollow follower: do not PUT a fabricated parent. */
    } else if (parent_row.layout == EFS_META_LAYOUT_LOCAL) {
        parent_row.nlink++;
        parent_row.base_mtime = max_u64(parent_row.base_mtime, at->now);
        parent_row.base_ctime = max_u64(parent_row.base_ctime, at->now);
        efs_meta_dir_note_entry(&parent_row, 1);
        touch_parent = 1;
    } else {
        parent_row.nlink++;
        bit = 1ull << efs_kv_dir_lane(name);
        if ((parent_row.used_shards & bit) == 0)
            parent_row.used_shards |= bit;
        rc = dir_lane_stamp(kv, &parent_row, name, at->now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
        touch_parent = 1;
    }
    pack_inode(v_ino, &row);
    pack_dentry(v_dent, &dent);
    be64(v_alloc, next);
    rc = efs_kv_key_dentry(dsh, parent, name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(csh, ino, k_ino, &ki);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(csh, k_alloc, &ka);
    if (rc == EFS_OK)
        rc = alloc_key_claim(kv, k_alloc, ka, k_aver, &kav, v_aver);
    if (rc == EFS_OK && touch_parent) {
        pack_inode(v_par, &parent_row);
        rc = efs_kv_key_inode(psh, parent, k_par, &kp);
    }
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
    it[n].op = EFS_KV_PUT;
    it[n].key = k_aver;
    it[n].klen = kav;
    it[n].val = v_aver;
    it[n].vlen = 8;
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
    rc = dseq_bump(kv, dsh, parent, dseq_lane, k_dseq, &ks, v_dseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
    n++;
    rc = efs_kv_batch(kv, it, n);
    if (rc != EFS_OK)
        return rc;
    if (out)
        *out = ino;
    return EFS_OK;
}

int efs_meta_apply_mkdir(struct efs_kv *kv, const struct efs_meta_attrs *at,
                         efs_ino_t parent, uint32_t mode, const char *name,
                         efs_ino_t *out)
{
    return mkdir_batch(kv, at, parent, mode, name, 0, 0, -1, out);
}

int efs_meta_apply_mkdir_at(struct efs_kv *kv, const struct efs_meta_attrs *at,
                            efs_ino_t parent, uint32_t mode, const char *name,
                            efs_ino_t ino, uint8_t layout, efs_ino_t *out)
{
    if (!ino)
        return EFS_ERR_INVAL;
    return mkdir_batch(kv, at, parent, mode, name, 1, ino, (int)layout, out);
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
    uint8_t k_reap[EFS_KV_KEY_MAX], v_reap[EFS_META_REAP_VAL];
    uint32_t kl = 0, kh = 0, ki = 0, kp = 0, kln = 0, kr = 0;
    struct efs_kv_item it[8];
    uint32_t n = 0, psh, hsh;
    int rc, held = 0;
    int touch_parent = 0;
    int stamp_lane = 0;
    uint8_t k_dseq[EFS_KV_KEY_MAX], v_dseq[8];
    uint32_t ks = 0;
    uint8_t dseq_lane;

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
        /* A transaction in the middle of changing this row (a LINK's
         * nlink++ reduce, a RENAME's parent SET) must not lose to an
         * unversioned DEL — same no-wait rule as PREPARE: BUSY, retry. */
        rc = efs_txn_key_busy(kv, k_ino, ki);
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
            /* The row dies here; the reap marker is what the background
             * reaper needs to sweep the dead file's lanes and GC its
             * fragments (L7), and it must be atomic with the delete. The
             * lease-held case above keeps the row, so the marker comes
             * from efs_meta_apply_reclaim when the last lease closes. */
            rc = efs_kv_key_reap(
                efs_kv_anchor_shard(efs_kv_inode_shard(row.ino)), row.ino,
                k_reap, &kr);
            if (rc != EFS_OK)
                return rc;
            efs_meta_pack_reap(v_reap, row.generation, row.active_lanes);
            it[n].op = EFS_KV_PUT;
            it[n].key = k_reap;
            it[n].klen = kr;
            it[n].val = v_reap;
            it[n].vlen = EFS_META_REAP_VAL;
            n++;
        }
    } else {
        /* Surviving links keep the inode; POSIX updates its ctime. */
        row.nlink--;
        row.base_ctime = max_u64(row.base_ctime, now);
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
        pack_inode(v_ino, &row);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ino;
        it[n].klen = ki;
        it[n].val = v_ino;
        it[n].vlen = INO_VAL;
        n++;
    }
    /* Same split as create: LOCAL times ride the parent row; spread times
     * live on the dentry shard's dir lane so unlink does not re-serialize
     * on the directory's home leader. */
    if (prow.layout == EFS_META_LAYOUT_LOCAL) {
        prow.base_mtime = max_u64(prow.base_mtime, now);
        prow.base_ctime = max_u64(prow.base_ctime, now);
        efs_meta_dir_note_entry(&prow, -1);
        touch_parent = 1;
    } else {
        efs_dir_spread_seen(&prow);
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
    dseq_lane = prow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(name);
    rc = dseq_bump(kv, efs_kv_dentry_shard(parent, name, prow.layout), parent,
                   dseq_lane, k_dseq, &ks, v_dseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
    n++;
    return efs_kv_batch(kv, it, n);
}

int efs_meta_apply_link(struct efs_kv *kv, efs_ino_t src_parent, const char *src_name,
                        efs_ino_t dst_parent, const char *dst_name, uint64_t now)
{
    struct efs_meta_dentry src, ndent;
    struct efs_meta_row row, dprow;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t k_par[EFS_KV_KEY_MAX], k_ln[EFS_KV_KEY_MAX];
    uint8_t k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[DENT_VAL], v_ino[INO_VAL], v_par[INO_VAL];
    uint8_t v_ln[LANE_VAL], v_dseq[8];
    uint32_t kd = 0, ki = 0, kp = 0, kln = 0, ks = 0;
    struct efs_kv_item it[8];
    uint32_t n = 0, dsh;
    uint64_t bit;
    uint8_t dseq_lane;
    int rc;
    int touch_parent = 0;
    int stamp_lane = 0;

    if (!kv || !src_name || !dst_name || src_parent == 0 || dst_parent == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_resolve(kv, src_parent, src_name, &src, &row);
    if (rc != EFS_OK)
        return rc;
    if (S_ISDIR(row.mode))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, dst_parent, &dprow);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(dprow.mode))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_lookup(kv, dst_parent, dst_name, &ndent);
    if (rc == EFS_OK)
        return EFS_ERR_EXIST;
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;

    dsh = efs_kv_dentry_shard(dst_parent, dst_name, dprow.layout);
    dseq_lane = dprow.layout == EFS_META_LAYOUT_LOCAL ? 0
                                                     : efs_kv_dir_lane(dst_name);
    memset(&ndent, 0, sizeof(ndent));
    ndent.ino = row.ino;
    ndent.generation = row.generation;
    ndent.type = row.mode & S_IFMT;
    pack_dentry(v_dent, &ndent);
    row.nlink++;
    row.base_ctime = max_u64(row.base_ctime, now);
    pack_inode(v_ino, &row);

    rc = efs_kv_key_dentry(dsh, dst_parent, dst_name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;

    if (dprow.layout == EFS_META_LAYOUT_LOCAL) {
        dprow.base_mtime = max_u64(dprow.base_mtime, now);
        dprow.base_ctime = max_u64(dprow.base_ctime, now);
        efs_meta_dir_note_entry(&dprow, 1);
        touch_parent = 1;
    } else {
        efs_dir_spread_seen(&dprow);
        bit = 1ull << efs_kv_dir_lane(dst_name);
        if ((dprow.used_shards & bit) == 0) {
            dprow.used_shards |= bit;
            touch_parent = 1;
        }
        rc = dir_lane_stamp(kv, &dprow, dst_name, now, k_ln, &kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        stamp_lane = 1;
    }
    if (touch_parent) {
        pack_inode(v_par, &dprow);
        rc = efs_kv_key_inode(efs_kv_inode_shard(dst_parent), dst_parent, k_par,
                              &kp);
        if (rc != EFS_OK)
            return rc;
    }
    rc = dseq_bump(kv, dsh, dst_parent, dseq_lane, k_dseq, &ks, v_dseq);
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
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
    n++;
    return efs_kv_batch(kv, it, n);
}

/* Drop one name from a directory into `it`, matching unlink's LOCAL /
 * SPLITTING / HASHED rules. `k_loc`/`k_hash`/`v_tomb` must outlive the batch. */
static int dentry_drop_items(struct efs_kv *kv, const struct efs_meta_row *prow,
                             efs_ino_t parent, const char *name,
                             uint8_t *k_loc, uint32_t *kl, uint8_t *k_hash,
                             uint32_t *kh, uint8_t *v_tomb,
                             struct efs_kv_item *it, uint32_t *n)
{
    uint32_t psh, hsh;
    struct efs_meta_dentry tomb;
    int rc;

    (void)kv;
    psh = efs_kv_inode_shard(parent);
    hsh = efs_kv_dentry_shard(parent, name, EFS_META_LAYOUT_HASHED);
    rc = efs_kv_key_dentry(psh, parent, name, k_loc, kl);
    if (rc == EFS_OK)
        rc = efs_kv_key_dentry(hsh, parent, name, k_hash, kh);
    if (rc != EFS_OK)
        return rc;
    if (prow->layout != EFS_META_LAYOUT_HASHED &&
        !(prow->layout == EFS_META_LAYOUT_SPLITTING &&
          *kl == *kh && memcmp(k_loc, k_hash, *kl) == 0)) {
        it[*n].op = EFS_KV_DEL;
        it[*n].key = k_loc;
        it[*n].klen = *kl;
        (*n)++;
    }
    if (prow->layout == EFS_META_LAYOUT_SPLITTING) {
        memset(&tomb, 0, sizeof(tomb));
        tomb.generation = prow->layout_epoch;
        tomb.type = EFS_META_DENT_TOMBSTONE;
        pack_dentry(v_tomb, &tomb);
        it[*n].op = EFS_KV_PUT;
        it[*n].key = k_hash;
        it[*n].klen = *kh;
        it[*n].val = v_tomb;
        it[*n].vlen = DENT_VAL;
        (*n)++;
    } else if (prow->layout == EFS_META_LAYOUT_HASHED) {
        it[*n].op = EFS_KV_DEL;
        it[*n].key = k_hash;
        it[*n].klen = *kh;
        (*n)++;
    }
    return EFS_OK;
}

static int stamp_dir_items(struct efs_kv *kv, struct efs_meta_row *prow,
                           efs_ino_t parent, const char *name, uint64_t now,
                           uint8_t *k_par, uint32_t *kp, uint8_t *v_par,
                           uint8_t *k_ln, uint32_t *kln, uint8_t *v_ln,
                           int *touch_parent, int *stamp_lane)
{
    uint64_t bit;
    int rc;

    *touch_parent = 0;
    *stamp_lane = 0;
    if (prow->layout == EFS_META_LAYOUT_LOCAL) {
        prow->base_mtime = max_u64(prow->base_mtime, now);
        prow->base_ctime = max_u64(prow->base_ctime, now);
        *touch_parent = 1;
        /* Caller sets nents via efs_meta_dir_note_entry before stamp. */
    } else {
        bit = 1ull << efs_kv_dir_lane(name);
        if ((prow->used_shards & bit) == 0) {
            prow->used_shards |= bit;
            *touch_parent = 1;
        }
        rc = dir_lane_stamp(kv, prow, name, now, k_ln, kln, v_ln);
        if (rc != EFS_OK)
            return rc;
        *stamp_lane = 1;
    }
    if (*touch_parent) {
        pack_inode(v_par, prow);
        rc = efs_kv_key_inode(efs_kv_inode_shard(parent), parent, k_par, kp);
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_OK;
}

/* POSIX: src must not be an ancestor of dst. Walking the live tree is
 * unsound under concurrency; the sim txn guards parent_version. This
 * helper is the single-KV form of the same predicate. */
static int dir_is_under(struct efs_kv *kv, efs_ino_t ancestor, efs_ino_t start)
{
    efs_ino_t cur = start;
    struct efs_meta_row r;
    int hops, rc;

    for (hops = 0; hops < 64; hops++) {
        if (cur == ancestor)
            return EFS_ERR_INVAL;
        rc = efs_meta_apply_get_inode(kv, cur, &r);
        if (rc != EFS_OK)
            return rc;
        if (!S_ISDIR(r.mode))
            return EFS_ERR_INVAL;
        if (cur == EFS_ROOT_INO || cur == r.parent)
            return EFS_OK;
        cur = r.parent;
    }
    return EFS_ERR_INVAL;
}

int efs_meta_apply_rename(struct efs_kv *kv, efs_ino_t src_parent,
                          const char *src_name, efs_ino_t dst_parent,
                          const char *dst_name, uint64_t now)
{
    struct efs_meta_dentry src, exist;
    struct efs_meta_row row, sprow, dprow;
    uint8_t k_sloc[EFS_KV_KEY_MAX], k_shash[EFS_KV_KEY_MAX], v_stomb[DENT_VAL];
    uint8_t k_dent[EFS_KV_KEY_MAX], v_dent[DENT_VAL];
    uint8_t k_ino[EFS_KV_KEY_MAX], v_ino[INO_VAL];
    uint8_t k_spar[EFS_KV_KEY_MAX], v_spar[INO_VAL];
    uint8_t k_dpar[EFS_KV_KEY_MAX], v_dpar[INO_VAL];
    uint8_t k_sln[EFS_KV_KEY_MAX], v_sln[LANE_VAL];
    uint8_t k_dln[EFS_KV_KEY_MAX], v_dln[LANE_VAL];
    uint8_t k_sdseq[EFS_KV_KEY_MAX], v_sdseq[8];
    uint8_t k_ddseq[EFS_KV_KEY_MAX], v_ddseq[8];
    uint8_t k_pver[EFS_KV_KEY_MAX], v_pver[8];
    uint32_t ksl = 0, ksh = 0, kd = 0, ki = 0, ksp = 0, kdp = 0;
    uint32_t ksln = 0, kdln = 0, kss = 0, kds = 0, kpv = 0;
    struct efs_kv_item it[14];
    uint32_t n = 0, dsh;
    int rc, same_dir, is_dir, touch_src = 0, stamp_src = 0, touch_dst = 0,
        stamp_dst = 0;
    uint8_t s_lane, d_lane;

    if (!kv || !src_name || !dst_name || src_parent == 0 || dst_parent == 0)
        return EFS_ERR_INVAL;
    if (src_parent == dst_parent && strcmp(src_name, dst_name) == 0)
        return EFS_OK;
    rc = efs_meta_apply_resolve(kv, src_parent, src_name, &src, &row);
    if (rc != EFS_OK)
        return rc;
    is_dir = S_ISDIR(row.mode);
    if (is_dir) {
        if (row.ino == EFS_ROOT_INO)
            return EFS_ERR_INVAL;
        rc = dir_is_under(kv, row.ino, dst_parent);
        if (rc != EFS_OK)
            return rc;
        row.parent_version++;
    }
    rc = efs_meta_apply_get_inode(kv, src_parent, &sprow);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(kv, dst_parent, &dprow);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(dprow.mode))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_lookup(kv, dst_parent, dst_name, &exist);
    if (rc == EFS_OK)
        return EFS_ERR_EXIST; /* replace is a wider txn; not this helper */
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;

    same_dir = src_parent == dst_parent;
    if (is_dir && !same_dir) {
        if (sprow.nlink < 3)
            return EFS_ERR_PROTO;
        sprow.nlink--;
        dprow.nlink++;
    }
    dsh = efs_kv_dentry_shard(dst_parent, dst_name, dprow.layout);
    s_lane = sprow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(src_name);
    d_lane = dprow.layout == EFS_META_LAYOUT_LOCAL ? 0 : efs_kv_dir_lane(dst_name);
    memset(&exist, 0, sizeof(exist));
    exist.ino = row.ino;
    exist.generation = row.generation;
    exist.type = row.mode & S_IFMT;
    pack_dentry(v_dent, &exist);
    row.parent = dst_parent;
    row.base_ctime = max_u64(row.base_ctime, now);
    pack_inode(v_ino, &row);
    rc = efs_kv_key_dentry(dsh, dst_parent, dst_name, k_dent, &kd);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;

    memset(it, 0, sizeof(it));
    rc = dentry_drop_items(kv, &sprow, src_parent, src_name, k_sloc, &ksl,
                           k_shash, &ksh, v_stomb, it, &n);
    if (rc != EFS_OK)
        return rc;
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
    if (is_dir) {
        rc = efs_kv_key_pver(efs_kv_inode_shard(row.ino), row.ino, k_pver, &kpv);
        if (rc != EFS_OK)
            return rc;
        be64(v_pver, row.parent_version);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_pver;
        it[n].klen = kpv;
        it[n].val = v_pver;
        it[n].vlen = 8;
        n++;
    }

    /* Apply rename does not replace (EXIST). same-dir is net-zero nents. */
    if (!same_dir)
        efs_meta_dir_note_entry(&sprow, -1);
    efs_dir_spread_seen(&sprow);
    rc = stamp_dir_items(kv, &sprow, src_parent, src_name, now, k_spar, &ksp,
                         v_spar, k_sln, &ksln, v_sln, &touch_src, &stamp_src);
    if (rc != EFS_OK)
        return rc;
    if (!same_dir) {
        efs_meta_dir_note_entry(&dprow, 1);
        efs_dir_spread_seen(&dprow);
        rc = stamp_dir_items(kv, &dprow, dst_parent, dst_name, now, k_dpar, &kdp,
                             v_dpar, k_dln, &kdln, v_dln, &touch_dst,
                             &stamp_dst);
        if (rc != EFS_OK)
            return rc;
    } else {
        /* One parent row: times already in sprow. Dest name may land on
         * a dir lane this directory has never used — that is first use,
         * so the used_shards bit belongs on the same PUT as src. */
        touch_dst = 0;
        stamp_dst = 0;
        if (dprow.layout != EFS_META_LAYOUT_LOCAL) {
            uint64_t bit = 1ull << efs_kv_dir_lane(dst_name);
            if ((sprow.used_shards & bit) == 0) {
                sprow.used_shards |= bit;
                pack_inode(v_spar, &sprow);
                rc = efs_kv_key_inode(efs_kv_inode_shard(src_parent), src_parent,
                                      k_spar, &ksp);
                if (rc != EFS_OK)
                    return rc;
                touch_src = 1;
            }
            rc = dir_lane_stamp(kv, &sprow, dst_name, now, k_dln, &kdln, v_dln);
            if (rc != EFS_OK)
                return rc;
            stamp_dst = 1;
        }
    }
    if (is_dir && !same_dir) {
        if (!touch_src) {
            pack_inode(v_spar, &sprow);
            rc = efs_kv_key_inode(efs_kv_inode_shard(src_parent), src_parent,
                                  k_spar, &ksp);
            if (rc != EFS_OK)
                return rc;
            touch_src = 1;
        }
        if (!touch_dst) {
            pack_inode(v_dpar, &dprow);
            rc = efs_kv_key_inode(efs_kv_inode_shard(dst_parent), dst_parent,
                                  k_dpar, &kdp);
            if (rc != EFS_OK)
                return rc;
            touch_dst = 1;
        }
    }
    if (touch_src) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_spar;
        it[n].klen = ksp;
        it[n].val = v_spar;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_src) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_sln;
        it[n].klen = ksln;
        it[n].val = v_sln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    if (touch_dst) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_dpar;
        it[n].klen = kdp;
        it[n].val = v_dpar;
        it[n].vlen = INO_VAL;
        n++;
    }
    if (stamp_dst) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_dln;
        it[n].klen = kdln;
        it[n].val = v_dln;
        it[n].vlen = LANE_VAL;
        n++;
    }
    rc = dseq_bump(kv, efs_kv_dentry_shard(src_parent, src_name, sprow.layout),
                   src_parent, s_lane, k_sdseq, &kss, v_sdseq);
    if (rc == EFS_OK)
        rc = dseq_bump(kv, dsh, dst_parent, d_lane, k_ddseq, &kds, v_ddseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_sdseq;
    it[n].klen = kss;
    it[n].val = v_sdseq;
    it[n].vlen = 8;
    n++;
    if (!(same_dir && sprow.layout == EFS_META_LAYOUT_LOCAL &&
          kss == kds && memcmp(k_sdseq, k_ddseq, kss) == 0)) {
        it[n].op = EFS_KV_PUT;
        it[n].key = k_ddseq;
        it[n].klen = kds;
        it[n].val = v_ddseq;
        it[n].vlen = 8;
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
    struct efs_meta_dentry dent;
    struct efs_meta_row row, prow;
    uint8_t k_loc[EFS_KV_KEY_MAX], k_hash[EFS_KV_KEY_MAX], v_tomb[DENT_VAL];
    uint8_t k_ino[EFS_KV_KEY_MAX], k_par[EFS_KV_KEY_MAX], v_par[INO_VAL];
    uint8_t k_ln[EFS_KV_KEY_MAX], v_ln[LANE_VAL];
    uint8_t k_dseq[EFS_KV_KEY_MAX], v_dseq[8];
    uint32_t kl = 0, kh = 0, ki = 0, kp = 0, kln = 0, ks = 0;
    struct efs_kv_item it[8];
    uint32_t n = 0;
    int rc, held, stamp_lane = 0;
    uint8_t dseq_lane;

    if (!kv || !name || parent == 0)
        return EFS_ERR_INVAL;
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
    rc = efs_kv_key_inode(efs_kv_inode_shard(row.ino), row.ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_DEL;
    it[n].key = k_ino;
    it[n].klen = ki;
    n++;
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
    rc = dseq_bump(kv, efs_kv_dentry_shard(parent, name, prow.layout), parent,
                   dseq_lane, k_dseq, &ks, v_dseq);
    if (rc != EFS_OK)
        return rc;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_dseq;
    it[n].klen = ks;
    it[n].val = v_dseq;
    it[n].vlen = 8;
    n++;
    return efs_kv_batch(kv, it, n);
}

int efs_meta_apply_reclaim(struct efs_kv *kv, efs_ino_t ino)
{
    struct efs_meta_row row;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint8_t k_reap[EFS_KV_KEY_MAX], v_reap[EFS_META_REAP_VAL];
    uint32_t ki = 0, kr = 0;
    struct efs_kv_item it[2];
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
    if (rc == EFS_OK)
        rc = efs_kv_key_reap(efs_kv_anchor_shard(efs_kv_inode_shard(ino)),
                             ino, k_reap, &kr);
    if (rc != EFS_OK)
        return rc;
    /* Row delete and reap marker are one batch: the marker is the reaper's
     * only record of the dead file's lanes (L7), so it can never be lost
     * to a crash between the two writes. Append cursor/reservation records
     * are cleaned by REAP_DONE, keeping this hot entry small. */
    memset(it, 0, sizeof(it));
    it[0].op = EFS_KV_DEL;
    it[0].key = k_ino;
    it[0].klen = ki;
    efs_meta_pack_reap(v_reap, row.generation, row.active_lanes);
    it[1].op = EFS_KV_PUT;
    it[1].key = k_reap;
    it[1].klen = kr;
    it[1].val = v_reap;
    it[1].vlen = EFS_META_REAP_VAL;
    return efs_kv_batch(kv, it, 2);
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

/* True when a new publication carries the exact fragment set of the row it
 * supersedes — the raft truncate tail stub reuses the old row's placement
 * instead of writing new fragments. The fragments are shared with the live
 * row, not dead, so a CAS supersede between aliased rows must NOT queue a
 * GC record (the reaper would delete the only copy of the tail data). */
static int chunk_aliases(const struct efs_meta_chunk *a,
                         const struct efs_meta_chunk *b)
{
    return memcmp(a->nodes, b->nodes, sizeof(a->nodes)) == 0 &&
           memcmp(a->checksums, b->checksums, sizeof(a->checksums)) == 0;
}

void efs_meta_pack_gc(uint8_t *p, const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                      uint8_t ack_bits,
                      const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    int i;

    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        be32(p + (uint32_t)i * 4u, nodes[i]);
    p[4 * EFS_NUM_FRAGMENTS] = ack_bits;
    memset(p + 4 * EFS_NUM_FRAGMENTS + 1, 0, 3);
    memcpy(p + 4 * EFS_NUM_FRAGMENTS + 4, sums,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
}

int efs_meta_unpack_gc(const uint8_t *p, uint32_t n,
                       efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                       uint8_t *ack_bits,
                       uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    int i;

    if (!p || n < EFS_META_GC_VAL)
        return EFS_ERR_PROTO;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        nodes[i] = rd32(p + (uint32_t)i * 4u);
    *ack_bits = p[4 * EFS_NUM_FRAGMENTS];
    memcpy(sums, p + 4 * EFS_NUM_FRAGMENTS + 4,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
    return EFS_OK;
}

void efs_meta_pack_reap(uint8_t *p, uint64_t generation,
                        uint64_t active_lanes)
{
    be64(p, generation);
    be64(p + 8, active_lanes);
}

int efs_meta_unpack_reap(const uint8_t *p, uint32_t n, uint64_t *generation,
                         uint64_t *active_lanes)
{
    if (!p || n < EFS_META_REAP_VAL)
        return EFS_ERR_PROTO;
    *generation = rd64(p);
    *active_lanes = rd64(p + 8);
    return EFS_OK;
}

/* Queue one GC record for a dead chunk generation. The record lands on the
 * anchor shard of the chunk's lane shard, so the reaper scans one prefix
 * per group. gc_keys/gc_vals are caller storage indexed by the global item
 * number, same discipline as trunc_scan's del_keys. */
static int gc_queue(struct efs_kv_item *it, uint32_t *n, uint32_t cap,
                    uint8_t (*gc_keys)[EFS_KV_KEY_MAX],
                    uint8_t (*gc_vals)[EFS_META_GC_VAL],
                    efs_ino_t ino, uint8_t lane, uint32_t ci,
                    const struct efs_meta_chunk *dead)
{
    uint32_t kg = 0;
    int rc;

    if (*n >= cap)
        return EFS_ERR_NOMEM;
    rc = efs_kv_key_gc(efs_kv_anchor_shard(efs_kv_lane_shard(ino, lane)),
                       ino, dead->generation, lane, ci,
                       gc_keys[*n], &kg);
    if (rc != EFS_OK)
        return rc;
    efs_meta_pack_gc(gc_vals[*n], dead->nodes, 0, dead->checksums);
    it[*n].op = EFS_KV_PUT;
    it[*n].key = gc_keys[*n];
    it[*n].klen = kg;
    it[*n].val = gc_vals[*n];
    it[*n].vlen = EFS_META_GC_VAL;
    (*n)++;
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

#define APPEND_CUR_VAL 24
#define APPEND_RSV_VAL 48
#define APPEND_OPEN 0
#define APPEND_DONE 1

struct append_cur {
    uint64_t watermark;
    uint64_t frontier;
    uint32_t nopen;
};

struct append_rsv {
    uint64_t off;
    uint64_t len;
    uint8_t uuid[EFS_OPID_UUID_LEN];
    uint32_t epoch;
    uint64_t seq;
    uint8_t state;
};

static void pack_append_cur(uint8_t *p, const struct append_cur *c)
{
    be64(p + 0, c->watermark);
    be64(p + 8, c->frontier);
    be32(p + 16, c->nopen);
    memset(p + 20, 0, 4);
}

static int unpack_append_cur(const uint8_t *p, uint32_t n, struct append_cur *c)
{
    if (!p || !c || n < APPEND_CUR_VAL)
        return EFS_ERR_PROTO;
    memset(c, 0, sizeof(*c));
    c->watermark = rd64(p + 0);
    c->frontier = rd64(p + 8);
    c->nopen = rd32(p + 16);
    return EFS_OK;
}

static void pack_append_rsv(uint8_t *p, const struct append_rsv *r)
{
    be64(p + 0, r->off);
    be64(p + 8, r->len);
    memcpy(p + 16, r->uuid, EFS_OPID_UUID_LEN);
    be32(p + 32, r->epoch);
    be64(p + 36, r->seq);
    p[44] = r->state;
    memset(p + 45, 0, 3);
}

static int unpack_append_rsv(const uint8_t *p, uint32_t n, struct append_rsv *r)
{
    if (!p || !r || n < 45)
        return EFS_ERR_PROTO;
    memset(r, 0, sizeof(*r));
    r->off = rd64(p + 0);
    r->len = rd64(p + 8);
    memcpy(r->uuid, p + 16, EFS_OPID_UUID_LEN);
    r->epoch = rd32(p + 32);
    r->seq = rd64(p + 36);
    r->state = p[44];
    return EFS_OK;
}

static int load_append_cur(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                           struct append_cur *c)
{
    uint8_t key[EFS_KV_KEY_MAX], val[APPEND_CUR_VAL];
    uint32_t klen = 0, vlen;
    int rc;

    memset(c, 0, sizeof(*c));
    rc = efs_kv_key_append_cur(efs_kv_inode_shard(ino), ino, gen, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = efs_kv_get(kv, key, klen, val, &vlen);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    return unpack_append_cur(val, vlen, c);
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
    /* gc_queue indexes by the global item number, so these are cap-sized. */
    uint8_t k_gc[5][EFS_KV_KEY_MAX], v_gc[5][EFS_META_GC_VAL];
    uint32_t kc = 0, kl = 0, ki = 0, vn;
    uint64_t committed = 0;
    uint64_t inode_gen, mtime_gen, row_base_size = 0, row_active = 0;
    struct lane_rec ln;
    struct efs_kv_item it[5];
    uint32_t n = 0;
    int touch_inode = 0;
    int rc;

    if (!kv || !p || p->ino == 0)
        return EFS_ERR_INVAL;
    rc = evidence_ok(p);
    if (rc != EFS_OK)
        return rc;
    if (p->lane_local) {
        /* Cross-group lane: this group's KV has no inode row. The host read
         * the row under ReadIndex on the inode group and carries the FileID
         * fields; the lane's own fenced_epoch is the linearizable truncate
         * fence here. An orphan publish (the inode was unlinked between the
         * host's read and this apply) is harmless: the chunk/lane keys are
         * generation-scoped garbage for GC (P3/L7), never reachable because
         * the row that would reference them is gone. */
        if (p->inode_gen == 0)
            return EFS_ERR_INVAL;
        inode_gen = p->inode_gen;
        mtime_gen = p->mtime_gen;
        memset(&row, 0, sizeof(row));
    } else {
        rc = efs_meta_apply_get_inode(kv, p->ino, &row);
        /* P3: a publish racing an unlink is stale work — make it harmless.
         * The inode row is gone, so there is nothing to attach the chunk to;
         * the fragments are orphans for GC (L7). A no-op OK, never an error:
         * the client's create for this ino committed before it could dirty the
         * chunk, so NOT_FOUND after commit ordering means deleted, not
         * not-yet-created. */
        if (rc == EFS_ERR_NOT_FOUND)
            return EFS_OK;
        if (rc != EFS_OK)
            return rc;
        /* FUSE stores a symlink target as ordinary published bytes.
         * Directories have no chunk map. */
        if (!S_ISREG(row.mode) && !S_ISLNK(row.mode))
            return EFS_ERR_INVAL;
        if (p->content_epoch < row.content_epoch)
            return EFS_ERR_STALE;
        {
            struct append_cur cur;

            rc = load_append_cur(kv, p->ino, row.generation, &cur);
            if (rc != EFS_OK)
                return rc;
            if (cur.nopen && p->new_size > cur.watermark)
                return EFS_ERR_BUSY;
        }
        inode_gen = row.generation;
        mtime_gen = row.mtime_gen;
        row_base_size = row.base_size;
        row_active = row.active_lanes;
    }
    lane = (uint8_t)(p->chunk_index % EFS_META_LANES);
    lsh = efs_kv_lane_shard(p->ino, lane);
    ish = efs_kv_inode_shard(p->ino);
    rc = efs_kv_key_chunk(lsh, p->ino, inode_gen, lane, p->chunk_index,
                          k_ch, &kc);
    if (rc == EFS_OK)
        rc = efs_kv_key_lane(lsh, p->ino, inode_gen, lane, k_ln, &kl);
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
    if (ln.append_bar && p->new_size > ln.append_bar)
        return EFS_ERR_BUSY;
    ln.max_end = max_u64(ln.max_end, p->new_size);
    /* A write updates mtime AND ctime, and both live here rather than on the
     * inode row so that a million writers never touch the inode's leader.
     * MAX-clamped: CLOCK_REALTIME can step backwards, and re-applying a
     * committed entry must not move anything. */
    if (ln.mtime_gen < mtime_gen) {
        /* A utimens has invalidated this lane's mtime since it was stamped,
         * so it is not a value to take a MAX against — it is stale. */
        ln.max_mtime = p->now;
        ln.mtime_gen = mtime_gen;
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
     * life, which is why it is not a rate-proportional cost. When the lane
     * is not the inode's own shard it is a genuine two-shard write: the
     * bitmap bit is set by a separate EFS_MD_CMD_ACTIVATE_LANE entry on the
     * inode group (proposed by the host BEFORE this lane-local publish), and
     * this entry — applied on the lane's group — must not touch the row. */
    if (!p->lane_local) {
        if ((row_active & (1ULL << lane)) == 0) {
            row_active |= 1ULL << lane;
            row.active_lanes = row_active;
            touch_inode = 1;
        }
        if (lsh == ish && p->new_size > row_base_size) {
            row.base_size = p->new_size;
            touch_inode = 1;
        }
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
    /* CAS supersede: the previous generation's value is overwritten in
     * place (the chunk key carries the FileID generation, not the chunk
     * generation), so its fragment set would be lost without a GC record
     * (L7). Emit one for the superseded generation — unless the new row
     * aliases the old fragment set (truncate tail stub), in which case the
     * fragments are shared with the live row and nothing is dead. */
    if (committed != 0 && !chunk_aliases(&stored, &got)) {
        rc = gc_queue(it, &n, (uint32_t)(sizeof(it) / sizeof(it[0])),
                      k_gc, v_gc, p->ino, lane, p->chunk_index, &got);
        if (rc != EFS_OK)
            return rc;
    }
    return efs_kv_batch(kv, it, n);
}

int efs_meta_apply_activate_lanes(struct efs_kv *kv, efs_ino_t ino,
                                  uint64_t mask)
{
    struct efs_meta_row row;
    uint8_t key[EFS_KV_KEY_MAX], val[INO_VAL];
    uint32_t klen = 0;
    int rc;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    if (mask == 0)
        return EFS_OK;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    /* P3: the inode was unlinked between the host's read and this apply —
     * the activation is stale work, harmless to drop. */
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    if ((row.active_lanes & mask) == mask)
        return EFS_OK; /* idempotent replay */
    row.active_lanes |= mask;
    pack_inode(val, &row);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &klen);
    if (rc != EFS_OK)
        return rc;
    return efs_kv_put(kv, key, klen, val, INO_VAL);
}

int efs_meta_apply_activate_lane(struct efs_kv *kv, efs_ino_t ino,
                                 uint8_t lane)
{
    if (lane >= EFS_META_LANES)
        return EFS_ERR_INVAL;
    return efs_meta_apply_activate_lanes(kv, ino, 1ULL << lane);
}

static int epoch_lane_upd(struct lane_rec *ln, void *arg);
static int fence_lane_bits(struct efs_kv *kv, efs_ino_t ino,
                           uint64_t generation, uint64_t bits,
                           int (*upd)(struct lane_rec *ln, void *arg),
                           void *arg, struct efs_kv_item *it, uint32_t *n,
                           uint8_t k_ln[][EFS_KV_KEY_MAX],
                           uint8_t v_ln[][LANE_VAL]);
static int truncate_lane_range_del(struct efs_kv *kv, efs_ino_t ino,
                                   uint64_t gen, uint8_t lane, uint64_t size,
                                   uint32_t tail_ci, uint8_t has_tail,
                                   struct efs_kv_item *it,
                                   uint8_t (*del_keys)[EFS_KV_KEY_MAX],
                                   uint8_t (*gc_keys)[EFS_KV_KEY_MAX],
                                   uint8_t (*gc_vals)[EFS_META_GC_VAL],
                                   uint32_t *n, uint32_t cap);

int efs_meta_apply_lane_fence(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                              uint8_t lane, uint64_t new_epoch, uint64_t size,
                              uint32_t tail_ci, uint8_t has_tail)
{
    /* One lane fence PUT + up to 32 chunk DELs, each with a GC record.
     * gc_queue indexes by the global item number, so all arrays share the
     * same bound. */
    struct efs_kv_item it[1 + 32 + 32];
    uint8_t del_keys[1 + 32 + 32][EFS_KV_KEY_MAX];
    uint8_t gc_keys[1 + 32 + 32][EFS_KV_KEY_MAX];
    uint8_t gc_vals[1 + 32 + 32][EFS_META_GC_VAL];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    uint8_t key[EFS_KV_KEY_MAX], old[LANE_VAL];
    uint32_t n = 0, klen = 0, vn;
    struct lane_rec ln;
    int rc;

    if (!kv || ino == 0 || lane >= EFS_META_LANES)
        return EFS_ERR_INVAL;
    /* Idempotency: a replay of an already-applied fence (same or older
     * epoch) must not zero max_end again — a publish that landed after the
     * first apply would be silently dropped from the size collect. */
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane,
                         key, &klen);
    if (rc != EFS_OK)
        return rc;
    vn = sizeof(old);
    rc = efs_kv_get(kv, key, klen, old, &vn);
    if (rc == EFS_OK) {
        rc = unpack_lane(old, vn, &ln);
        if (rc != EFS_OK)
            return rc;
        if (ln.fenced_epoch >= new_epoch)
            return EFS_OK;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    rc = fence_lane_bits(kv, ino, gen, 1ULL << lane, epoch_lane_upd,
                         &new_epoch, it, &n, k_ln, v_ln);
    if (rc != EFS_OK)
        return rc;
    rc = truncate_lane_range_del(kv, ino, gen, lane, size, tail_ci, has_tail,
                                 it, del_keys, gc_keys, gc_vals, &n,
                                 (uint32_t)(sizeof(it) / sizeof(it[0])));
    if (rc != EFS_OK)
        return rc;
    if (n == 0)
        return EFS_OK;
    return efs_kv_batch(kv, it, n);
}

/* Lane sweep batch: 64 chunk keys per KV batch (each a DEL + a GC record),
 * then the lane key. 64 keeps one entry's apply cost bounded while still
 * draining a 1 TB file's lane (~8192 chunks) in ~128 batches inside the
 * single LANE_SWEEP entry — tens of ms, far under the election deadline. */
#define SWEEP_CHUNKS 64

struct sweep_scan {
    efs_ino_t ino;
    uint64_t gen;
    struct efs_kv_item *it;
    uint8_t (*keys)[EFS_KV_KEY_MAX];
    uint8_t (*gc_vals)[EFS_META_GC_VAL];
    uint32_t n;
    uint32_t cap;
    uint32_t chunks;
    int full;
    int rc;
};

static int sweep_cb(void *user, const uint8_t *key, uint32_t klen,
                    const uint8_t *val, uint32_t vlen)
{
    struct sweep_scan *ss = user;
    struct efs_meta_chunk dead;
    efs_ino_t ino;
    uint64_t gen;
    uint32_t ci;
    uint8_t lane;

    if (klen < 24 || key[2] != EFS_KV_KIND_CHUNK)
        return 0;
    ino = rd64(key + 3);
    gen = rd64(key + 11);
    lane = key[19];
    ci = rd32(key + 20);
    if (ino != ss->ino || gen != ss->gen)
        return 0;
    if (ss->n + 2 > ss->cap) {
        ss->full = 1;
        return 1;
    }
    if (unpack_chunk(val, vlen, &dead) != EFS_OK) {
        ss->rc = EFS_ERR_PROTO;
        return 1;
    }
    memcpy(ss->keys[ss->n], key, klen);
    ss->it[ss->n].op = EFS_KV_DEL;
    ss->it[ss->n].key = ss->keys[ss->n];
    ss->it[ss->n].klen = klen;
    ss->it[ss->n].val = NULL; /* `it` is an uninitialised stack array */
    ss->it[ss->n].vlen = 0;
    ss->n++;
    ss->rc = gc_queue(ss->it, &ss->n, ss->cap, ss->keys, ss->gc_vals,
                      ino, lane, ci, &dead);
    if (ss->rc != EFS_OK)
        return 1;
    ss->chunks++;
    return 0;
}

int efs_meta_apply_lane_sweep(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                              uint8_t lane)
{
    struct efs_kv_item it[SWEEP_CHUNKS * 2];
    uint8_t keys[SWEEP_CHUNKS * 2][EFS_KV_KEY_MAX];
    uint8_t gc_vals[SWEEP_CHUNKS * 2][EFS_META_GC_VAL];
    uint8_t pref[EFS_KV_KEY_MAX], k_ln[EFS_KV_KEY_MAX];
    uint32_t plen = 0, kl = 0;
    struct sweep_scan ss;
    uint32_t lsh;
    int rc;

    if (!kv || ino == 0 || lane >= EFS_META_LANES)
        return EFS_ERR_INVAL;
    lsh = efs_kv_lane_shard(ino, lane);
    rc = efs_kv_key_chunk(lsh, ino, gen, lane, 0, pref, &plen);
    if (rc == EFS_OK)
        rc = efs_kv_key_lane(lsh, ino, gen, lane, k_ln, &kl);
    if (rc != EFS_OK)
        return rc;
    plen = 20;
    for (;;) {
        memset(&ss, 0, sizeof(ss));
        ss.ino = ino;
        ss.gen = gen;
        ss.it = it;
        ss.keys = keys;
        ss.gc_vals = gc_vals;
        ss.cap = (uint32_t)(sizeof(it) / sizeof(it[0]));
        rc = efs_kv_scan_prefix(kv, pref, plen, sweep_cb, &ss);
        if (ss.rc != EFS_OK)
            return ss.rc == EFS_ERR_INVAL ? EFS_OK : ss.rc;
        if (rc == EFS_ERR_INVAL)
            return EFS_OK;
        /* rc > 0 is sweep_cb's "batch full, stop" (merge_scan propagates
         * the callback's return), not an error. Treating it as one made
         * every lane with more than SWEEP_CHUNKS chunks unsweepable: the
         * reaper re-proposed LANE_SWEEP once a second forever and no
         * fragment of any file over ~8 MiB per lane was ever reclaimed
         * (IO-500 ior-easy 1.2 GiB files: `apply lane-sweep rc=1` every
         * 33 entries). */
        if (rc < 0)
            return rc;
        if (ss.chunks == 0)
            break;
        rc = efs_kv_batch(kv, it, ss.n);
        if (rc != EFS_OK)
            return rc;
        if (!ss.full)
            break;
    }
    /* The lane key itself is the last thing to go. A replay finds no
     * chunks and re-deletes an absent lane key, which is a no-op. */
    rc = efs_kv_del(kv, k_ln, kl);
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK;
    return rc;
}

struct rsv_purge {
    struct efs_kv_item *it;
    uint8_t (*keys)[EFS_KV_KEY_MAX];
    uint32_t n;
    uint32_t cap;
    uint32_t found;
    int full;
};

static int rsv_purge_cb(void *user, const uint8_t *key, uint32_t klen,
                        const uint8_t *val, uint32_t vlen)
{
    struct rsv_purge *rp = user;

    (void)val;
    (void)vlen;
    if (rp->n >= rp->cap) {
        rp->full = 1;
        return 1;
    }
    memcpy(rp->keys[rp->n], key, klen);
    rp->it[rp->n].op = EFS_KV_DEL;
    rp->it[rp->n].key = rp->keys[rp->n];
    rp->it[rp->n].klen = klen;
    rp->it[rp->n].val = NULL;
    rp->it[rp->n].vlen = 0;
    rp->n++;
    rp->found++;
    return 0;
}

int efs_meta_apply_reap_done(struct efs_kv *kv, efs_ino_t ino, uint64_t gen)
{
    struct efs_meta_row row;
    struct efs_kv_item it[3];
    uint8_t k_reap[EFS_KV_KEY_MAX], k_cur[EFS_KV_KEY_MAX];
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint32_t kr = 0, kc = 0, ki = 0, n = 0;
    uint32_t ish;
    int rc, held, drop_row = 0;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    ish = efs_kv_inode_shard(ino);
    /* Defensive: the marker and the row delete commit in one batch, so a
     * live row here should be impossible. If one shows up anyway (a row
     * re-created under the same ino is not a thing that exists), the
     * marker is the stale part — drop it and keep the row. */
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_OK) {
        if (row.generation != gen || row.nlink != 0) {
            rc = efs_kv_key_reap(efs_kv_anchor_shard(ish), ino, k_reap, &kr);
            if (rc != EFS_OK)
                return rc;
            rc = efs_kv_del(kv, k_reap, kr);
            if (rc == EFS_ERR_NOT_FOUND)
                rc = EFS_OK;
            return rc;
        }
        held = efs_lease_any(kv, ino, row.generation);
        if (held < 0)
            return held;
        if (!held)
            drop_row = 1;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    /* Leftover append reservations of a killed appender. Bounded batches;
     * each batch's deletes make the next scan progress. */
    for (;;) {
        struct efs_kv_item rit[32];
        uint8_t rkeys[32][EFS_KV_KEY_MAX];
        uint8_t pref[EFS_KV_KEY_MAX];
        uint32_t plen = 0;
        struct rsv_purge rp;

        rc = efs_kv_key_append_rsv_prefix(ish, ino, gen, pref, &plen);
        if (rc != EFS_OK)
            return rc;
        memset(&rp, 0, sizeof(rp));
        rp.it = rit;
        rp.keys = rkeys;
        rp.cap = 32;
        rc = efs_kv_scan_prefix(kv, pref, plen, rsv_purge_cb, &rp);
        if (rc < 0) /* rc > 0 = callback's batch-full stop, not an error */
            return rc;
        if (rp.found == 0)
            break;
        rc = efs_kv_batch(kv, rit, rp.n);
        if (rc != EFS_OK)
            return rc;
        if (!rp.full)
            break;
    }
    rc = efs_kv_key_append_cur(ish, ino, gen, k_cur, &kc);
    if (rc == EFS_OK)
        rc = efs_kv_key_reap(efs_kv_anchor_shard(ish), ino, k_reap, &kr);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_DEL;
    it[n].key = k_cur;
    it[n].klen = kc;
    n++;
    it[n].op = EFS_KV_DEL;
    it[n].key = k_reap;
    it[n].klen = kr;
    n++;
    if (drop_row) {
        rc = efs_kv_key_inode(ish, ino, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
        it[n].op = EFS_KV_DEL;
        it[n].key = k_ino;
        it[n].klen = ki;
        n++;
    }
    return efs_kv_batch(kv, it, n);
}

int efs_meta_apply_gc_ack(struct efs_kv *kv, const struct efs_gc_ack_item *it,
                          uint32_t n)
{
    struct efs_kv_item batch[16];
    uint8_t keys[16][EFS_KV_KEY_MAX];
    uint8_t vals[16][EFS_META_GC_VAL];
    uint8_t retired[16];
    uint32_t i, j, bn = 0;
    int rc;

    if (!kv || (!it && n))
        return EFS_ERR_INVAL;
    if (n > 16)
        return EFS_ERR_INVAL;
    memset(retired, 0, sizeof(retired));
    for (i = 0; i < n; i++) {
        efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
        uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        uint8_t ack_bits;
        uint32_t kg = 0, vn;
        uint8_t key[EFS_KV_KEY_MAX];
        uint8_t old[EFS_META_GC_VAL];

        if (it[i].lane >= EFS_META_LANES || it[i].frag >= EFS_NUM_FRAGMENTS)
            return EFS_ERR_INVAL;
        rc = efs_kv_key_gc(efs_kv_anchor_shard(
                               efs_kv_lane_shard(it[i].ino, it[i].lane)),
                           it[i].ino, it[i].gen, it[i].lane, it[i].ci,
                           key, &kg);
        if (rc != EFS_OK)
            return rc;
        /* One batch usually carries every fragment of the same record;
         * fold into the pending entry — a fresh KV read cannot see the
         * bits the earlier items of this batch have not applied yet. */
        for (j = 0; j < bn; j++) {
            if (batch[j].klen == kg && memcmp(batch[j].key, key, kg) == 0)
                break;
        }
        if (j < bn) {
            if (retired[j])
                continue;
            rc = efs_meta_unpack_gc(vals[j], EFS_META_GC_VAL, nodes,
                                    &ack_bits, sums);
            if (rc != EFS_OK)
                return rc;
            ack_bits |= (uint8_t)(1u << it[i].frag);
            if (ack_bits == (uint8_t)((1u << EFS_NUM_FRAGMENTS) - 1u)) {
                batch[j].op = EFS_KV_DEL;
                batch[j].val = NULL;
                batch[j].vlen = 0;
                retired[j] = 1;
            } else {
                efs_meta_pack_gc(vals[j], nodes, ack_bits,
                                 (const uint8_t (*)[EFS_HASH_SIZE])sums);
            }
            continue;
        }
        vn = sizeof(old);
        rc = efs_kv_get(kv, key, kg, old, &vn);
        /* Already retired (a replay of this entry): skip. */
        if (rc == EFS_ERR_NOT_FOUND)
            continue;
        if (rc != EFS_OK)
            return rc;
        rc = efs_meta_unpack_gc(old, vn, nodes, &ack_bits, sums);
        if (rc != EFS_OK)
            return rc;
        ack_bits |= (uint8_t)(1u << it[i].frag);
        memcpy(keys[bn], key, kg);
        batch[bn].key = keys[bn];
        batch[bn].klen = kg;
        if (ack_bits == (uint8_t)((1u << EFS_NUM_FRAGMENTS) - 1u)) {
            batch[bn].op = EFS_KV_DEL;
            batch[bn].val = NULL;
            batch[bn].vlen = 0;
            retired[bn] = 1;
        } else {
            efs_meta_pack_gc(vals[bn], nodes, ack_bits,
                             (const uint8_t (*)[EFS_HASH_SIZE])sums);
            batch[bn].op = EFS_KV_PUT;
            batch[bn].val = vals[bn];
            batch[bn].vlen = EFS_META_GC_VAL;
        }
        bn++;
    }
    if (bn == 0)
        return EFS_OK;
    return efs_kv_batch(kv, batch, bn);
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

        if (!is_dir) {
            struct append_cur cur;

            rc = load_append_cur(kv, ino, row.generation, &cur);
            if (rc != EFS_OK)
                return rc;
            if (cur.nopen)
                size = cur.frontier;
        }

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
    uint8_t (*gc_keys)[EFS_KV_KEY_MAX];
    uint8_t (*gc_vals)[EFS_META_GC_VAL];
    uint32_t n;
    uint32_t cap;
    int rc;
};

static int trunc_del_cb(void *user, const uint8_t *key, uint32_t klen,
                        const uint8_t *val, uint32_t vlen)
{
    struct trunc_scan *ts = user;
    struct efs_meta_chunk dead;
    efs_ino_t ino;
    uint64_t gen;
    uint32_t ci;
    uint8_t lane;

    if (klen < 24 || key[2] != EFS_KV_KIND_CHUNK)
        return 0;
    ino = rd64(key + 3);
    gen = rd64(key + 11);
    lane = key[19];
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
    if (ts->n + 2 > ts->cap) {
        ts->rc = EFS_ERR_NOMEM;
        return 1;
    }
    /* del_keys/gc_keys/gc_vals are indexed by the GLOBAL item number, never
     * a per-lane base: the scan runs once per lane over one shared array,
     * and a per-lane base would let lane i+1 overwrite keys that lane i's
     * items still point at — deleting the wrong chunks on a multi-lane
     * truncate. */
    memcpy(ts->del_keys[ts->n], key, klen);
    ts->it[ts->n].op = EFS_KV_DEL;
    ts->it[ts->n].key = ts->del_keys[ts->n];
    ts->it[ts->n].klen = klen;
    ts->n++;
    /* The chunk key dies here; its fragment set must not (L7). */
    if (unpack_chunk(val, vlen, &dead) != EFS_OK) {
        ts->rc = EFS_ERR_PROTO;
        return 1;
    }
    ts->rc = gc_queue(ts->it, &ts->n, ts->cap, ts->gc_keys, ts->gc_vals,
                      ino, lane, ci, &dead);
    if (ts->rc != EFS_OK)
        return 1;
    return 0;
}

static int truncate_lane_range_del(struct efs_kv *kv, efs_ino_t ino,
                                   uint64_t gen, uint8_t lane, uint64_t size,
                                   uint32_t tail_ci, uint8_t has_tail,
                                   struct efs_kv_item *it,
                                   uint8_t (*del_keys)[EFS_KV_KEY_MAX],
                                   uint8_t (*gc_keys)[EFS_KV_KEY_MAX],
                                   uint8_t (*gc_vals)[EFS_META_GC_VAL],
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
    ts.gc_keys = gc_keys;
    ts.gc_vals = gc_vals;
    ts.n = *n;
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
                                 uint8_t (*k_gc)[EFS_KV_KEY_MAX],
                                 uint8_t (*v_gc)[EFS_META_GC_VAL],
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
    if (*n + 3 > cap)
        return EFS_ERR_NOMEM;
    it[*n].op = EFS_KV_PUT;
    it[*n].key = k_ch;
    it[*n].klen = kc;
    it[*n].val = v_ch;
    it[*n].vlen = CHUNK_VAL;
    (*n)++;
    /* The straddling tail is CAS-published inside the truncate txn; the
     * generation it replaces is dead from this commit on (L7) — unless the
     * stub aliases the old row's fragment set, in which case the fragments
     * are shared with the live row and nothing is dead. */
    if (committed != 0 && !chunk_aliases(&stored, &got)) {
        rc = gc_queue(it, n, cap, k_gc, v_gc, row->ino, lane,
                      p.chunk_index, &got);
        if (rc != EFS_OK)
            return rc;
    }
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

/* Batch capacity: one fence per lane + the inode row + up to 32
 * range-deleted chunk keys per lane, each paired with a GC record (L7),
 * + the tail chunk/lane pair and its GC record. del_keys/gc_keys/gc_vals
 * carry the same bound and are indexed by the global item number (see
 * trunc_del_cb), so one `n + 2 > cap` check covers all arrays. */
#define TRUNC_IT_CAP (EFS_META_LANES + 1 + EFS_META_LANES * 32 * 2 + 3)

static int truncate_apply(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                          const struct efs_meta_truncate *t,
                          struct efs_kv_item *it,
                          uint8_t (*del_keys)[EFS_KV_KEY_MAX],
                          uint8_t (*gc_keys)[EFS_KV_KEY_MAX],
                          uint8_t (*gc_vals)[EFS_META_GC_VAL])
{
    struct efs_meta_row row;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint8_t v_ino[INO_VAL];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    uint8_t k_tail[EFS_KV_KEY_MAX], v_tail[CHUNK_VAL];
    uint32_t ki = 0, n = 0, i, tail_ci = 0;
    uint64_t new_epoch;
    uint8_t has_tail = 0;
    int touch_inode = 0;
    int rc;

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
        if (!has_tail && !t->tail_external)
            return EFS_ERR_INVAL;
        if (has_tail && t->tail->chunk_index != tail_ci)
            return EFS_ERR_INVAL;
        if (t->tail_external && t->tail != NULL)
            return EFS_ERR_INVAL;
    }
    new_epoch = row.content_epoch + 1;
    row.content_epoch = new_epoch;
    row.base_size = t->size;
    row.base_mtime = max_u64(row.base_mtime, now);
    row.base_ctime = max_u64(row.base_ctime, now);
    memset(it, 0, TRUNC_IT_CAP * sizeof(it[0]));
    memset(k_ln, 0, sizeof(k_ln));
    memset(v_ln, 0, sizeof(v_ln));
    for (i = 0; i < EFS_META_LANES; i++) {
        if ((row.active_lanes & t->lane_mask & (1ULL << i)) == 0)
            continue;
        rc = fence_lane_bits(kv, ino, row.generation, 1ULL << i, epoch_lane_upd,
                             &new_epoch, it, &n, k_ln, v_ln);
        if (rc != EFS_OK)
            return rc;
        rc = truncate_lane_range_del(kv, ino, row.generation, (uint8_t)i,
                                   t->size, tail_ci, has_tail, it, del_keys,
                                   gc_keys, gc_vals, &n, TRUNC_IT_CAP);
        if (rc != EFS_OK)
            return rc;
    }
    if (!t->tail_external) {
        /* The tail's GC record shares the cap-sized gc arrays: gc_queue
         * indexes them by the global item number, so single-entry stack
         * buffers here would be scribbled on at offset *n. */
        rc = truncate_publish_tail(kv, &row, now, t->tail, it, &n,
                                   TRUNC_IT_CAP, k_tail, v_tail,
                                   gc_keys, gc_vals, k_ln, v_ln);
        if (rc != EFS_OK)
            return rc;
    }
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

int efs_meta_apply_truncate(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                            const struct efs_meta_truncate *t)
{
    struct efs_kv_item *it;
    uint8_t (*del_keys)[EFS_KV_KEY_MAX];
    uint8_t (*gc_keys)[EFS_KV_KEY_MAX];
    uint8_t (*gc_vals)[EFS_META_GC_VAL];
    int rc;

    if (!kv || !t || ino == 0)
        return EFS_ERR_INVAL;
    /* ~1.6 MiB of batch storage belongs on the heap, not the stack: efsd
     * applies entries on a 1 MiB pump thread (efsd_pthread_create), where
     * this as a stack frame sits one field away from the guard page. */
    it = malloc(TRUNC_IT_CAP * sizeof(*it));
    del_keys = malloc(TRUNC_IT_CAP * sizeof(*del_keys));
    gc_keys = malloc(TRUNC_IT_CAP * sizeof(*gc_keys));
    gc_vals = malloc(TRUNC_IT_CAP * sizeof(*gc_vals));
    if (!it || !del_keys || !gc_keys || !gc_vals) {
        free(it);
        free(del_keys);
        free(gc_keys);
        free(gc_vals);
        return EFS_ERR_NOMEM;
    }
    rc = truncate_apply(kv, ino, now, t, it, del_keys, gc_keys, gc_vals);
    free(it);
    free(del_keys);
    free(gc_keys);
    free(gc_vals);
    return rc;
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

static int append_bar_set(struct lane_rec *ln, void *arg)
{
    uint64_t w = *(const uint64_t *)arg;

    if (ln->append_bar == w)
        return 0;
    ln->append_bar = w;
    return 1;
}

static int append_bar_clr(struct lane_rec *ln, void *arg)
{
    (void)arg;
    if (ln->append_bar == 0)
        return 0;
    ln->append_bar = 0;
    return 1;
}

static int collect_phys_eof(struct efs_kv *kv, const struct efs_meta_row *row,
                            efs_txn_coord_fn coord, void *ctx, uint64_t *eof,
                            uint64_t seqs[EFS_META_LANES])
{
    uint64_t size = row->base_size;
    uint32_t i;
    int rc;

    memset(seqs, 0, sizeof(uint64_t) * EFS_META_LANES);
    for (i = 0; i < EFS_META_LANES; i++) {
        uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
        uint32_t kl = 0, vn;
        struct efs_txn_reduce red;
        struct efs_txn_pending pend;
        struct lane_rec ln;

        if ((row->active_lanes & (1ULL << i)) == 0)
            continue;
        rc = efs_kv_key_lane(efs_kv_lane_shard(row->ino, (uint8_t)i), row->ino,
                             row->generation, (uint8_t)i, key, &kl);
        if (rc != EFS_OK)
            return rc;
        memset(&pend, 0, sizeof(pend));
        memset(&red, 0, sizeof(red));
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
        seqs[i] = ln.seq;
        size = max_u64(size, red.max_end);
        size = max_u64(size, ln.max_end);
    }
    *eof = size;
    return EFS_OK;
}

/* Reservation records accumulate while any reservation stays open (a killed
 * appender strands its OPEN records, and DONE records behind them are kept
 * until nopen reaches 0), so the scan grows: the old fixed 64-cap NOMEM'd
 * every resolve/report for the inode, which failed the client's whole
 * report batch and left the rec dirty — poisoning every later sync report
 * (the direct-io/symlink EIO window in the posix gate). */
struct rsv_scan {
    struct append_rsv *r;
    uint32_t n, cap;
    int rc;
};

static int rsv_scan_cb(void *user, const uint8_t *key, uint32_t klen,
                       const uint8_t *val, uint32_t vlen)
{
    struct rsv_scan *s = user;

    (void)key;
    (void)klen;
    if (s->n >= s->cap) {
        uint32_t ncap = s->cap ? s->cap * 2 : 64;
        struct append_rsv *nr = realloc(s->r, (size_t)ncap * sizeof(*nr));

        if (!nr) {
            s->rc = EFS_ERR_NOMEM;
            return 1;
        }
        s->r = nr;
        s->cap = ncap;
    }
    s->rc = unpack_append_rsv(val, vlen, &s->r[s->n]);
    if (s->rc != EFS_OK)
        return 1;
    s->n++;
    return 0;
}

static int load_rsvs(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                     struct rsv_scan *s)
{
    uint8_t pref[EFS_KV_KEY_MAX];
    uint32_t plen = 0;
    int rc;

    memset(s, 0, sizeof(*s));
    rc = efs_kv_key_append_rsv_prefix(efs_kv_inode_shard(ino), ino, gen, pref,
                                      &plen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_scan_prefix(kv, pref, plen, rsv_scan_cb, s);
    if (s->rc != EFS_OK)
        return s->rc;
    return rc;
}

/* 0 = ok, 1 = other uuid has OPEN, <0 = error. */
static int append_foreign_open(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                               const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    struct rsv_scan scan;
    uint32_t i;
    int rc, foreign = 0;

    rc = load_rsvs(kv, ino, gen, &scan);
    if (rc != EFS_OK) {
        free(scan.r);
        return rc;
    }
    for (i = 0; i < scan.n; i++) {
        if (scan.r[i].state != APPEND_OPEN)
            continue;
        if (memcmp(scan.r[i].uuid, uuid, EFS_OPID_UUID_LEN) != 0) {
            foreign = 1;
            break;
        }
    }
    free(scan.r);
    return foreign;
}

int efs_meta_apply_append_foreign(struct efs_kv *kv, efs_ino_t ino,
                                  const uint8_t uuid[EFS_OPID_UUID_LEN])
{
    struct efs_meta_row row;
    struct append_cur cur;
    int rc;

    if (!kv || !uuid || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    rc = load_append_cur(kv, ino, row.generation, &cur);
    if (rc != EFS_OK)
        return rc;
    if (cur.nopen == 0)
        return 0;
    return append_foreign_open(kv, ino, row.generation, uuid);
}

int efs_meta_apply_append_reserve(struct efs_kv *kv, efs_ino_t ino, uint64_t len,
                                  const struct efs_opid *op,
                                  efs_txn_coord_fn coord, void *ctx,
                                  uint64_t *off_out)
{
    struct efs_meta_row row;
    struct efs_opid_window win;
    struct efs_opid_reply rep;
    struct append_cur cur;
    struct append_rsv rsv;
    uint64_t seqs[EFS_META_LANES], seq2, phys, eof;
    uint8_t k_cur[EFS_KV_KEY_MAX], k_rsv[EFS_KV_KEY_MAX], k_opid[EFS_KV_KEY_MAX];
    uint8_t v_cur[APPEND_CUR_VAL], v_rsv[APPEND_RSV_VAL];
    uint8_t v_opid[EFS_OPID_VAL_MAX];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    struct efs_kv_item it[EFS_META_LANES + 4];
    uint32_t kc = 0, kr = 0, ko = 0, vo, n = 0, i;
    int rc, hit;

    if (!kv || !op || !coord || !off_out || ino == 0 || len == 0)
        return EFS_ERR_INVAL;
    hit = 0;
    if (opid_hosted(op)) {
        rc = load_window(kv, op, &win);
        if (rc != EFS_OK)
            return rc;
        hit = efs_opid_lookup(&win, op, &rep);
        if (hit < 0)
            return hit;
        if (hit) {
            *off_out = rep.extra;
            return rep.rc;
        }
    }
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISREG(row.mode))
        return EFS_ERR_INVAL;
    rc = collect_phys_eof(kv, &row, coord, ctx, &phys, seqs);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < EFS_META_LANES; i++) {
        if ((row.active_lanes & (1ULL << i)) == 0)
            continue;
        rc = lane_seq_get(kv, ino, row.generation, (uint8_t)i, &seq2);
        if (rc != EFS_OK)
            return rc;
        if (seq2 != seqs[i])
            return EFS_ERR_BUSY;
    }
    rc = load_append_cur(kv, ino, row.generation, &cur);
    if (rc != EFS_OK)
        return rc;
    if (cur.nopen) {
        rc = append_foreign_open(kv, ino, row.generation, op->client_uuid);
        if (rc > 0)
            return EFS_ERR_BUSY;
        if (rc < 0)
            return rc;
    }
    eof = max_u64(phys, cur.watermark);
    if (cur.nopen == 0)
        cur.frontier = eof;
    cur.watermark = eof + len;
    cur.nopen++;
    memset(&rsv, 0, sizeof(rsv));
    rsv.off = eof;
    rsv.len = len;
    memcpy(rsv.uuid, op->client_uuid, EFS_OPID_UUID_LEN);
    rsv.epoch = op->session_epoch;
    rsv.seq = op->seq;
    rsv.state = APPEND_OPEN;
    pack_append_cur(v_cur, &cur);
    pack_append_rsv(v_rsv, &rsv);
    rc = efs_kv_key_append_cur(efs_kv_inode_shard(ino), ino, row.generation,
                               k_cur, &kc);
    if (rc == EFS_OK)
        rc = efs_kv_key_append_rsv(efs_kv_inode_shard(ino), ino, row.generation,
                                   eof, k_rsv, &kr);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_cur;
    it[n].klen = kc;
    it[n].val = v_cur;
    it[n].vlen = APPEND_CUR_VAL;
    n++;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_rsv;
    it[n].klen = kr;
    it[n].val = v_rsv;
    it[n].vlen = APPEND_RSV_VAL;
    n++;
    rc = fence_lane_bits(kv, ino, row.generation, row.active_lanes,
                         append_bar_set, &cur.watermark, it, &n, k_ln, v_ln);
    if (rc != EFS_OK)
        return rc;
    if (opid_hosted(op)) {
        memset(&rep, 0, sizeof(rep));
        rep.rc = EFS_OK;
        rep.ino = ino;
        rep.extra = eof;
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
    *off_out = eof;
    return EFS_OK;
}

int efs_meta_apply_append_resolve(struct efs_kv *kv, efs_ino_t ino, uint64_t off,
                                  int outcome)
{
    struct efs_meta_row row;
    struct append_cur cur;
    struct append_rsv rsv;
    struct rsv_scan scan;
    uint8_t k_cur[EFS_KV_KEY_MAX], k_rsv[EFS_KV_KEY_MAX], k_ino[EFS_KV_KEY_MAX];
    uint8_t v_cur[APPEND_CUR_VAL], v_rsv[APPEND_RSV_VAL], v_ino[INO_VAL];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    uint8_t (*del_keys)[EFS_KV_KEY_MAX] = NULL;
    uint32_t *del_kl = NULL;
    struct efs_kv_item *it = NULL;
    uint32_t kc = 0, kr = 0, ki = 0, n = 0, i;
    int rc, hole, progressed = 1;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    if (outcome != EFS_META_APPEND_COMPLETED &&
        outcome != EFS_META_APPEND_ABORTED_HOLE &&
        outcome != EFS_META_APPEND_FENCED_HOLE)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_key_append_rsv(efs_kv_inode_shard(ino), ino, row.generation, off,
                               k_rsv, &kr);
    if (rc != EFS_OK)
        return rc;
    {
        uint8_t old[APPEND_RSV_VAL];
        uint32_t vn = sizeof(old);

        rc = efs_kv_get(kv, k_rsv, kr, old, &vn);
        if (rc == EFS_ERR_NOT_FOUND) {
            rc = load_append_cur(kv, ino, row.generation, &cur);
            if (rc != EFS_OK)
                return rc;
            /* Drain deletes every rsv. A retry after that is a no-op;
             * a bogus offset while the burst is still live is not. */
            return cur.nopen ? EFS_ERR_NOT_FOUND : EFS_OK;
        }
        if (rc != EFS_OK)
            return rc;
        rc = unpack_append_rsv(old, vn, &rsv);
        if (rc != EFS_OK)
            return rc;
    }
    if (rsv.state == APPEND_DONE)
        return EFS_OK;
    hole = (outcome != EFS_META_APPEND_COMPLETED);
    rsv.state = APPEND_DONE;
    rc = load_append_cur(kv, ino, row.generation, &cur);
    if (rc != EFS_OK)
        return rc;
    if (cur.nopen == 0)
        return EFS_ERR_PROTO;
    cur.nopen--;
    if (hole)
        row.base_size = max_u64(row.base_size, rsv.off + rsv.len);
    rc = load_rsvs(kv, ino, row.generation, &scan);
    if (rc != EFS_OK) {
        free(scan.r);
        return rc;
    }
    for (i = 0; i < scan.n; i++) {
        if (scan.r[i].off == off)
            scan.r[i].state = APPEND_DONE;
    }
    while (progressed) {
        progressed = 0;
        for (i = 0; i < scan.n; i++) {
            if (scan.r[i].state == APPEND_DONE &&
                scan.r[i].off == cur.frontier) {
                cur.frontier = scan.r[i].off + scan.r[i].len;
                progressed = 1;
                scan.r[i].state = 0xff;
            }
        }
    }
    it = malloc(((size_t)scan.n + EFS_META_LANES + 4) * sizeof(*it));
    del_keys = malloc((size_t)(scan.n ? scan.n : 1) * sizeof(*del_keys));
    del_kl = malloc((size_t)(scan.n ? scan.n : 1) * sizeof(*del_kl));
    if (!it || !del_keys || !del_kl) {
        rc = EFS_ERR_NOMEM;
        goto out;
    }
    memset(it, 0, ((size_t)scan.n + EFS_META_LANES + 4) * sizeof(*it));
    rc = efs_kv_key_append_cur(efs_kv_inode_shard(ino), ino, row.generation,
                               k_cur, &kc);
    if (rc != EFS_OK)
        goto out;
    if (cur.nopen == 0) {
        row.base_size = max_u64(row.base_size, cur.frontier);
        it[n].op = EFS_KV_DEL;
        it[n].key = k_cur;
        it[n].klen = kc;
        n++;
        if (scan.n == 0) {
            it[n].op = EFS_KV_DEL;
            it[n].key = k_rsv;
            it[n].klen = kr;
            n++;
        }
        for (i = 0; i < scan.n; i++) {
            rc = efs_kv_key_append_rsv(efs_kv_inode_shard(ino), ino,
                                       row.generation, scan.r[i].off,
                                       del_keys[i], &del_kl[i]);
            if (rc != EFS_OK)
                goto out;
            it[n].op = EFS_KV_DEL;
            it[n].key = del_keys[i];
            it[n].klen = del_kl[i];
            n++;
        }
        rc = fence_lane_bits(kv, ino, row.generation, row.active_lanes,
                             append_bar_clr, NULL, it, &n, k_ln, v_ln);
        if (rc != EFS_OK)
            goto out;
    } else {
        pack_append_rsv(v_rsv, &rsv);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_rsv;
        it[n].klen = kr;
        it[n].val = v_rsv;
        it[n].vlen = APPEND_RSV_VAL;
        n++;
        pack_append_cur(v_cur, &cur);
        it[n].op = EFS_KV_PUT;
        it[n].key = k_cur;
        it[n].klen = kc;
        it[n].val = v_cur;
        it[n].vlen = APPEND_CUR_VAL;
        n++;
    }
    pack_inode(v_ino, &row);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc != EFS_OK)
        goto out;
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    rc = efs_kv_batch(kv, it, n);
out:
    free(scan.r);
    free(del_keys);
    free(del_kl);
    free(it);
    return rc;
}

int efs_meta_apply_append_state(struct efs_kv *kv, efs_ino_t ino,
                                uint64_t *watermark, uint64_t *frontier,
                                uint32_t *nopen)
{
    struct efs_meta_row row;
    struct append_cur cur;
    int rc;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    rc = load_append_cur(kv, ino, row.generation, &cur);
    if (rc != EFS_OK)
        return rc;
    if (watermark)
        *watermark = cur.watermark;
    if (frontier)
        *frontier = cur.frontier;
    if (nopen)
        *nopen = cur.nopen;
    return EFS_OK;
}

int efs_meta_apply_append_open(struct efs_kv *kv, efs_ino_t ino, uint64_t *offs,
                               uint64_t *lens, uint32_t *n)
{
    struct efs_meta_row row;
    struct rsv_scan scan;
    uint32_t i, cap, outn = 0;
    int rc;

    if (!kv || !n || ino == 0)
        return EFS_ERR_INVAL;
    cap = *n;
    *n = 0;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    rc = load_rsvs(kv, ino, row.generation, &scan);
    if (rc != EFS_OK) {
        free(scan.r);
        return rc;
    }
    for (i = 0; i < scan.n; i++) {
        if (scan.r[i].state != APPEND_OPEN)
            continue;
        if (outn < cap && offs && lens) {
            offs[outn] = scan.r[i].off;
            lens[outn] = scan.r[i].len;
        }
        outn++;
    }
    *n = (cap && outn > cap) ? cap : outn;
    free(scan.r);
    return EFS_OK;
}

#define DROP_RSV_MAX 32

struct drop_rsv_acc {
    const uint8_t *uuid;
    uint32_t epoch;
    efs_ino_t ino[DROP_RSV_MAX];
    uint64_t off[DROP_RSV_MAX];
    int n;
    int full;
};

static int drop_rsv_cb(void *user, const uint8_t *key, uint32_t klen,
                       const uint8_t *val, uint32_t vlen)
{
    struct drop_rsv_acc *a = user;
    struct append_rsv rsv;
    int rc;

    if (klen < 27 || !val)
        return 0;
    rc = unpack_append_rsv(val, vlen, &rsv);
    if (rc != EFS_OK)
        return 0;
    if (rsv.state != APPEND_OPEN)
        return 0;
    if (rsv.epoch != a->epoch)
        return 0;
    if (memcmp(rsv.uuid, a->uuid, EFS_OPID_UUID_LEN) != 0)
        return 0;
    if (a->n >= DROP_RSV_MAX) {
        a->full = 1;
        return 1;
    }
    a->ino[a->n] = rd64(key + 3);
    a->off[a->n] = rd64(key + 19);
    a->n++;
    return 0;
}

int efs_meta_apply_append_drop_session(struct efs_kv *kv, uint32_t shard,
                                       const uint8_t uuid[EFS_OPID_UUID_LEN],
                                       uint32_t epoch)
{
    uint8_t pref[EFS_KV_KEY_MAX];
    uint32_t plen = 0;
    int rc;

    if (!kv || !uuid || shard > EFS_KV_SHARD_MASK)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_append_rsv_shard_prefix(shard, pref, &plen);
    if (rc != EFS_OK)
        return rc;
    for (;;) {
        struct drop_rsv_acc acc;
        int i;

        memset(&acc, 0, sizeof(acc));
        acc.uuid = uuid;
        acc.epoch = epoch;
        rc = efs_kv_scan_prefix(kv, pref, plen, drop_rsv_cb, &acc);
        if (rc != EFS_OK && rc != 1)
            return rc;
        if (acc.n == 0)
            return EFS_OK;
        for (i = 0; i < acc.n; i++) {
            rc = efs_meta_apply_append_resolve(kv, acc.ino[i], acc.off[i],
                                               EFS_META_APPEND_FENCED_HOLE);
            if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND)
                return rc;
        }
        if (!acc.full)
            return EFS_OK;
    }
}

/* Last-close edge: a reservation still OPEN when the inode's final lease
 * closes can never complete (the writer that reserved it is gone — killed
 * mid-append), so resolve them all as holes and let the append frontier and
 * base_size settle (§7.3 ABORTED_HOLE). Without this a killed appender
 * strands OPEN records forever: the frontier wedges and the records
 * accumulate without bound. Published data is unaffected either way — the
 * chunk CAS already landed; this is frontier bookkeeping. */
int efs_meta_apply_append_drain_file(struct efs_kv *kv, efs_ino_t ino)
{
    struct efs_meta_row row;
    struct rsv_scan scan;
    uint32_t i;
    int rc, rc2;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    rc = load_rsvs(kv, ino, row.generation, &scan);
    if (rc != EFS_OK) {
        free(scan.r);
        return rc;
    }
    rc = EFS_OK;
    for (i = 0; i < scan.n; i++) {
        if (scan.r[i].state != APPEND_OPEN)
            continue;
        rc2 = efs_meta_apply_append_resolve(kv, ino, scan.r[i].off,
                                            EFS_META_APPEND_ABORTED_HOLE);
        if (rc2 != EFS_OK && rc2 != EFS_ERR_NOT_FOUND && rc == EFS_OK)
            rc = rc2;
    }
    free(scan.r);
    return rc;
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
