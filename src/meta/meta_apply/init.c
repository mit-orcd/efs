/* meta_apply/init.c: inode allocation, opid records, mkfs/init, salt and
 * membership. */

#include "apply_internal.h"
#include "init.h"
#include "rows.h"

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

/* A deleted inode can retain a transaction version even when its row is
 * absent. Reusing it as a generation-1 allocation both revives an old FileID
 * and makes cross-group MKDIR's CAS(0) fail forever. Treat that durable
 * history as occupied when a stale allocator watermark or hint reaches it. */
static int alloc_inode_unused(struct efs_kv *kv, efs_ino_t ino, int *unused)
{
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t length = 0;
    uint64_t version = 0;
    int rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &length);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(kv, key, length, &version);
    if (rc == EFS_OK)
        *unused = version == 0;
    return rc;
}

/* Watermark can sit on a live row (SNAP lag, hollow replica, or a
 * rewind after unlink of a later sibling). Reusing that ino overwrites
 * the row and inherits leftover children — POSIX mkdir then EEXIST on
 * well-known names like p/c. Skip occupied inodes. */
#define ALLOC_SKIP_MAX 4096

int alloc_next_free(struct efs_kv *kv, uint32_t shard, efs_ino_t *ino,
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
            int unused = 0;
            rc = alloc_inode_unused(kv, next, &unused);
            if (rc != EFS_OK)
                return rc;
            if (unused) {
                *ino = next;
                *next_out = next + (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
                return EFS_OK;
            }
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
int alloc_hint_or_next(struct efs_kv *kv, uint32_t shard,
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
            int unused = 0;
            rc = alloc_inode_unused(kv, want, &unused);
            if (rc != EFS_OK)
                return rc;
            if (unused) {
                *ino = want;
                *next_out = want + (efs_ino_t)(1u << EFS_KV_SHARD_BITS);
                return EFS_OK;
            }
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
int alloc_key_claim(struct efs_kv *kv, const uint8_t *k_alloc,
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

/* Op-id window (I16, §7.9) for a directory op lives on the shard that
 * served it — the dentry shard — under efs_kv_key_opid(shard, uuid,
 * epoch). A zero UUID / seq 0 means "no identity": only server-internal
 * work (expiry) proposes such entries. Client RPCs without a session are
 * refused at the host boundary (K5), and every apply that takes an identity
 * checks it with efs_session_accept. */
int opid_hosted(const struct efs_opid *op)
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

static int opid_get(struct efs_kv *kv, uint32_t shard, const struct efs_opid *id,
                    uint8_t *key, uint32_t *klen, uint8_t *val, uint32_t *vlen)
{
    int rc;

    rc = efs_kv_key_opid(shard, id->client_uuid, id->session_epoch, key, klen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, key, *klen, val, vlen);
    if (rc == EFS_ERR_NOT_FOUND) {
        *vlen = 0;
        return EFS_OK;
    }
    return rc;
}

int efs_meta_apply_opid_probe(struct efs_kv *kv, uint32_t shard,
                              const struct efs_opid *id,
                              struct efs_opid_reply *out)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_OPID_VAL_MAX];
    uint32_t klen = 0, vlen = sizeof(val);
    int rc;

    if (!kv || !opid_hosted(id))
        return 0;
    rc=efs_session_accept(kv,shard,id->client_uuid,id->session_epoch);
    if(rc!=EFS_OK)return rc;
    rc = opid_get(kv, shard, id, key, &klen, val, &vlen);
    if (rc != EFS_OK)
        return rc;
    return efs_opid_probe(val, vlen, id, out);
}

/* Fill the batch item that records `rep` for q in the window on `shard`.
 * 1 = item filled; 0 = nothing to record (no identity, or the window has
 * no room — the op still applies); negative = KV error. */
int opid_item(struct efs_kv *kv, uint32_t shard,
                     const struct efs_opid_req *q, const struct efs_opid_reply *rep,
                     uint8_t *key, uint8_t *val, struct efs_kv_item *it)
{
    uint8_t cur[EFS_OPID_VAL_MAX];
    uint32_t klen = 0, cn = sizeof(cur), vlen = EFS_OPID_VAL_MAX;
    int rc;

    if (!q || !opid_hosted(&q->id))
        return 0;
    rc=efs_session_accept(kv,shard,q->id.client_uuid,q->id.session_epoch);
    if(rc!=EFS_OK)return rc;
    rc = opid_get(kv, shard, &q->id, key, &klen, cur, &cn);
    if (rc != EFS_OK)
        return rc;
    rc = efs_opid_fold(cur, cn, q->id.client_uuid, q->id.session_epoch, q, rep,
                       val, &vlen);
    if (rc == EFS_ERR_NOMEM)
        return 0;
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(*it));
    it->op = EFS_KV_PUT;
    it->key = key;
    it->klen = klen;
    it->val = val;
    it->vlen = vlen;
    return 1;
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
    return meta_write_batch(kv, it, 3);
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
    return meta_write_put(kv, k, kl, v, 8);
}

int efs_meta_membership_get(struct efs_kv *kv, uint32_t anchor, uint8_t *out,
                            uint32_t cap, uint32_t *len)
{
    uint8_t k[EFS_KV_KEY_MAX];
    uint32_t kl = 0;
    int rc;

    if (!kv || !out || !len)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_membership(anchor, k, &kl);
    if (rc != EFS_OK)
        return rc;
    *len = cap;
    return efs_kv_get(kv, k, kl, out, len);
}

int efs_meta_apply_membership_record(struct efs_kv *kv, uint32_t anchor,
                                     const uint8_t *rec, uint32_t len, int mode)
{
    uint8_t k[EFS_KV_KEY_MAX], have[EFS_MEMBERSHIP_MAX_LEN];
    uint32_t kl = 0, hl = 0;
    struct efs_membership want, cur;
    int rc;

    if (!kv || !rec || !len || len > EFS_MEMBERSHIP_MAX_LEN ||
        efs_membership_decode(rec, len, &want) != EFS_OK)
        return EFS_ERR_INVAL;
    rc = efs_meta_membership_get(kv, anchor, have, sizeof(have), &hl);
    if (rc == EFS_OK) {
        if (mode == EFS_MEMBERSHIP_KEEP_FIRST)
            return EFS_OK;
        if (efs_membership_decode(have, hl, &cur) != EFS_OK)
            return EFS_ERR_PROTO;
        if (efs_membership_extends(&cur, &want))
            return EFS_OK;
        if (!efs_membership_extends(&want, &cur) ||
            (mode == EFS_MEMBERSHIP_EXTEND_ONE && want.count != cur.count + 1))
            return EFS_ERR_FENCED;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    } else if (mode == EFS_MEMBERSHIP_EXTEND_ONE) {
        return EFS_ERR_UNFORMED;
    }
    rc = efs_kv_key_membership(anchor, k, &kl);
    if (rc != EFS_OK)
        return rc;
    return meta_write_put(kv, k, kl, rec, len);
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

int efs_meta_apply_alloc_prepare(struct efs_kv *kv, const struct efs_txid *t,
                                  const struct efs_txn_parts *parts,
                                  const uint8_t *key, uint32_t klen)
{
    if (!key || klen!=3 || key[2]!=EFS_KV_KIND_ALLOC ||
        (((uint32_t)key[0]<<8)|key[1])>EFS_KV_SHARD_MASK) return EFS_ERR_PROTO;
    uint32_t shard=((uint32_t)key[0]<<8)|key[1], n=8;
    uint8_t old[8], value[8];
    int rc=efs_txn_owned_excl(kv,t,parts,key,klen,value,&n);
    if (rc==EFS_OK) {
        /* Idempotent retry keeps its original reservation. */
        if (n!=8 || rd64(value)<(1u<<EFS_KV_SHARD_BITS) ||
            efs_kv_inode_shard(rd64(value))!=shard) return EFS_ERR_PROTO;
        return EFS_OK;
    }
    if (rc!=EFS_ERR_NOT_FOUND) return rc;
    efs_ino_t ino, next;
    rc=alloc_next_free(kv,shard,&ino,&next);
    if (rc!=EFS_OK) return rc;
    if (next<=ino) return EFS_ERR_PROTO;
    n=sizeof(old); rc=efs_kv_get(kv,key,klen,old,&n);
    if (rc==EFS_ERR_NOT_FOUND) n=EFS_TXN_ABSENT;
    else if (rc!=EFS_OK) return rc;
    else if (n!=sizeof(old)) return EFS_ERR_PROTO;
    be64(value,next);
    return efs_txn_prepare_excl_value(kv,t,parts,key,klen,old,n,EFS_TXN_PUT,value,sizeof(value));
}
