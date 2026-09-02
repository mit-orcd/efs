/* Second metadata Raft group + MKDIR as a 2-shard txn (§10 step 7).
 * Odd shards stay on GROUP_SHARD (root = 1); even shards use GROUP_SHARD2
 * so a scattering MKDIR has two independent logs (I17). */
#include "sim_internal.h"
#include "efs/kv_key.h"
#include "efs/txn.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CMD_PREPARE 5
#define CMD_DECIDE  6
#define CMD_RESOLVE 7
#define CMD_DROP    8
#define CMD_MAX     512
#define WAIT_BOOT   80

#define TXN_PREPARE  1
#define TXN_DECISION 2
#define TXN_RESOLVE  3

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)(v >> 32));
    wr32(p + 4, (uint32_t)v);
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

static int raft2_apply(void *app, uint64_t index, uint64_t term,
                       const uint8_t *cmd, uint32_t clen)
{
    (void)term;
    if (cmd && clen && cmd[0] == SIM_CMD_SESSION)
        return sim_sess_apply(app, EFS_RAFT_GROUP_SHARD2, cmd, clen, index);
    return sim_txn_apply(app, EFS_RAFT_GROUP_SHARD2, cmd, clen, index);
}

static int attach2_real(struct efs_sim *sim, int i)
{
    struct efs_raft_cfg cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.id = i;
    cfg.n = EFS_SIM_RAFT_N;
    cfg.voters = (1u << EFS_SIM_RAFT_N) - 1;
    cfg.boot_id = sim->srv[i].boot_id ? sim->srv[i].boot_id : 1;
    cfg.group = EFS_RAFT_GROUP_SHARD2;
    cfg.election_ticks = (uint32_t)(5 + i * 3);
    cfg.heartbeat_ticks = 1;
    cfg.store = sim->srv[i].raft2_store;
    cfg.store_ctx = sim->srv[i].raft2_store;
    cfg.send = sim_raft_send;
    cfg.net = sim;
    cfg.apply = raft2_apply;
    cfg.app = &sim->srv[i];
    sim->srv[i].sim = sim;
    sim->srv[i].raft2 = efs_raft_new(&cfg);
    return sim->srv[i].raft2 ? EFS_OK : EFS_ERR_NOMEM;
}

int sim_txn_boot(struct efs_sim *sim)
{
    int i, t, rc;

    if (!sim)
        return EFS_ERR_INVAL;
    for (i = 0; i < EFS_SIM_RAFT_N; i++) {
        sim->srv[i].raft2_store = efs_raft_mem_create();
        if (!sim->srv[i].raft2_store)
            return EFS_ERR_NOMEM;
        rc = attach2_real(sim, i);
        if (rc != EFS_OK)
            return rc;
    }
    for (t = 0; t < WAIT_BOOT; t++) {
        int n = 0;
        for (i = 0; i < EFS_SIM_RAFT_N; i++) {
            if (sim->srv[i].raft2 &&
                efs_raft_role(sim->srv[i].raft2) == EFS_RAFT_LEADER)
                n++;
        }
        if (n == 1)
            return EFS_OK;
        for (i = 0; i < EFS_SIM_RAFT_N; i++) {
            if (sim->srv[i].raft2)
                efs_raft_tick(sim->srv[i].raft2);
        }
    }
    return EFS_OK;
}

void sim_txn_halt(struct efs_sim *sim, int server)
{
    if (!sim || server < 0 || server >= EFS_SIM_RAFT_N)
        return;
    efs_raft_free(sim->srv[server].raft2);
    sim->srv[server].raft2 = NULL;
}

int sim_txn_restart(struct efs_sim *sim, int server)
{
    if (!sim || server < 0)
        return EFS_ERR_INVAL;
    if (server >= EFS_SIM_RAFT_N)
        return EFS_OK;
    if (sim->srv[server].raft2)
        return EFS_OK;
    if (!sim->srv[server].raft2_store)
        return EFS_ERR_INVAL;
    return attach2_real(sim, server);
}

void sim_txn_free_all(struct efs_sim *sim)
{
    int i;

    if (!sim)
        return;
    for (i = 0; i < EFS_SIM_MAX_SERVERS; i++) {
        efs_raft_free(sim->srv[i].raft2);
        sim->srv[i].raft2 = NULL;
        efs_raft_mem_free(sim->srv[i].raft2_store);
        sim->srv[i].raft2_store = NULL;
    }
}

int sim_txn_tick(struct efs_sim *sim, int server)
{
    if (!sim || server < 0 || server >= EFS_SIM_RAFT_N)
        return EFS_OK;
    if (!sim->srv[server].raft2)
        return EFS_OK;
    return efs_raft_tick(sim->srv[server].raft2);
}

int sim_txn_apply(struct sim_server *s, uint8_t group, const uint8_t *cmd,
                  uint32_t clen, uint64_t index)
{
    struct efs_txid t;
    uint32_t shard, off;
    int rc = EFS_ERR_PROTO, dec, kind, op;
    uint64_t expected;
    uint8_t klen;
    const uint8_t *key, *val;
    uint32_t vlen;
    struct efs_txn_parts p;
    struct efs_txn_reduce red;

    if (!s || !s->disk || !cmd || clen == 0)
        return EFS_OK;
    memset(&t, 0, sizeof(t));
    memset(&p, 0, sizeof(p));
    switch (cmd[0]) {
    case CMD_PREPARE:
        if (clen < 1 + 1 + 16 + 1)
            break;
        kind = cmd[1];
        memcpy(t.bytes, cmd + 2, 16);
        p.n = cmd[18];
        if (p.n == 0 || p.n > EFS_TXN_MAX_PART)
            break;
        off = 19;
        if (clen < off + (uint32_t)p.n * 4u + 1u)
            break;
        {
            uint8_t i;
            for (i = 0; i < p.n; i++)
                p.shard[i] = rd32(cmd + off + (uint32_t)i * 4u);
        }
        off += (uint32_t)p.n * 4u;
        klen = cmd[off++];
        if (clen < off + klen)
            break;
        key = cmd + off;
        off += klen;
        if (kind == EFS_TXN_EXCL) {
            if (clen < off + 8 + 1 + 4)
                break;
            expected = rd64(cmd + off);
            op = cmd[off + 8];
            vlen = rd32(cmd + off + 9);
            off += 13;
            if (clen < off + vlen)
                break;
            val = cmd + off;
            rc = efs_txn_prepare_excl(s->disk, &t, &p, key, klen, expected, op,
                                      val, vlen);
        } else if (kind == EFS_TXN_GUARD) {
            if (clen < off + 8)
                break;
            expected = rd64(cmd + off);
            rc = efs_txn_prepare_guard(s->disk, &t, &p, key, klen, expected);
        } else if (kind == EFS_TXN_REDUCE) {
            if (clen < off + 24)
                break;
            red.max_end = rd64(cmd + off);
            red.max_mtime = rd64(cmd + off + 8);
            red.max_ctime = rd64(cmd + off + 16);
            rc = efs_txn_prepare_reduce(s->disk, &t, &p, key, klen, &red);
        }
        break;
    case CMD_DECIDE:
        if (clen < 1 + 16 + 4 + 1)
            break;
        memcpy(t.bytes, cmd + 1, 16);
        shard = rd32(cmd + 17);
        dec = cmd[21];
        rc = efs_txn_decide(s->disk, shard, &t, dec);
        break;
    case CMD_RESOLVE:
        if (clen < 1 + 16 + 4 + 1)
            break;
        memcpy(t.bytes, cmd + 1, 16);
        shard = rd32(cmd + 17);
        dec = cmd[21];
        rc = efs_txn_resolve(s->disk, &t, shard, dec);
        break;
    case CMD_DROP:
        if (clen < 1 + 16 + 4)
            break;
        memcpy(t.bytes, cmd + 1, 16);
        shard = rd32(cmd + 17);
        rc = efs_txn_drop(s->disk, &t, shard);
        break;
    default:
        rc = EFS_ERR_PROTO;
        break;
    }
    sim_note_apply(s, group, index, rc, 0);
    return EFS_OK;
}

static uint32_t pack_prep(uint8_t *out, int kind, const struct efs_txid *t,
                          const struct efs_txn_parts *p, const uint8_t *key,
                          uint32_t klen, uint64_t expected, int op,
                          const uint8_t *val, uint32_t vlen,
                          const struct efs_txn_reduce *red)
{
    uint32_t n, i;

    out[0] = CMD_PREPARE;
    out[1] = (uint8_t)kind;
    memcpy(out + 2, t->bytes, 16);
    out[18] = p->n;
    n = 19;
    for (i = 0; i < p->n; i++) {
        wr32(out + n, p->shard[i]);
        n += 4;
    }
    out[n++] = (uint8_t)klen;
    memcpy(out + n, key, klen);
    n += klen;
    if (kind == EFS_TXN_EXCL) {
        wr64(out + n, expected);
        out[n + 8] = (uint8_t)op;
        wr32(out + n + 9, vlen);
        n += 13;
        if (vlen) {
            memcpy(out + n, val, vlen);
            n += vlen;
        }
    } else if (kind == EFS_TXN_GUARD) {
        wr64(out + n, expected);
        n += 8;
    } else if (kind == EFS_TXN_REDUCE && red) {
        wr64(out + n, red->max_end);
        wr64(out + n + 8, red->max_mtime);
        wr64(out + n + 16, red->max_ctime);
        n += 24;
    }
    return n;
}

static int propose_prep(struct efs_sim *sim, uint32_t shard, int kind,
                        const struct efs_txid *t, const struct efs_txn_parts *p,
                        const uint8_t *key, uint32_t klen, uint64_t expected,
                        int op, const uint8_t *val, uint32_t vlen,
                        const struct efs_txn_reduce *red)
{
    uint8_t cmd[CMD_MAX];
    uint32_t n;

    n = pack_prep(cmd, kind, t, p, key, klen, expected, op, val, vlen, red);
    if (n > CMD_MAX)
        return EFS_ERR_INVAL;
    return sim_raft_propose_group(sim, sim_shard_group(shard), cmd, n);
}

static int propose_decide(struct efs_sim *sim, uint32_t coord, const struct efs_txid *t,
                          int dec)
{
    uint8_t cmd[22];

    cmd[0] = CMD_DECIDE;
    memcpy(cmd + 1, t->bytes, 16);
    wr32(cmd + 17, coord);
    cmd[21] = (uint8_t)dec;
    return sim_raft_propose_group(sim, sim_shard_group(coord), cmd, 22);
}

static int propose_resolve(struct efs_sim *sim, uint32_t shard,
                           const struct efs_txid *t, int dec)
{
    uint8_t cmd[22];

    cmd[0] = CMD_RESOLVE;
    memcpy(cmd + 1, t->bytes, 16);
    wr32(cmd + 17, shard);
    cmd[21] = (uint8_t)dec;
    return sim_raft_propose_group(sim, sim_shard_group(shard), cmd, 22);
}

static int propose_drop(struct efs_sim *sim, uint32_t shard, const struct efs_txid *t)
{
    uint8_t cmd[21];

    cmd[0] = CMD_DROP;
    memcpy(cmd + 1, t->bytes, 16);
    wr32(cmd + 17, shard);
    return sim_raft_propose_group(sim, sim_shard_group(shard), cmd, 21);
}

struct coord_ctx {
    struct efs_sim *sim;
};

static int sim_coord(void *user, const struct efs_txid *t, uint32_t coord_shard,
                     int *dec)
{
    struct coord_ctx *c = user;
    struct efs_kv *kv;
    int rc;

    rc = sim_raft_read_group(c->sim, sim_shard_group(coord_shard));
    if (rc != EFS_OK)
        return EFS_ERR_IO;
    kv = sim_raft_kv_group(c->sim, sim_shard_group(coord_shard));
    if (!kv)
        return EFS_ERR_IO;
    return efs_txn_decision_get(kv, coord_shard, t, dec);
}

int sim_txn_lookup(struct efs_sim *sim, efs_ino_t parent, const char *name,
                   struct efs_meta_dentry *out)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_META_DENT_BYTES];
    uint32_t klen = 0, vlen = EFS_META_DENT_BYTES;
    uint32_t shard;
    struct efs_kv *kv;
    struct coord_ctx ctx;
    int rc;

    if (!sim || !name || !out || parent == 0)
        return EFS_ERR_INVAL;
    shard = efs_kv_inode_shard(parent);
    rc = sim_raft_read_group(sim, sim_shard_group(shard));
    if (rc != EFS_OK)
        return rc;
    kv = sim_raft_kv_group(sim, sim_shard_group(shard));
    if (!kv)
        return EFS_ERR_BUSY;
    rc = efs_kv_key_dentry(shard, parent, name, key, &klen);
    if (rc != EFS_OK)
        return rc;
    ctx.sim = sim;
    rc = efs_txn_read(kv, key, klen, sim_coord, &ctx, val, &vlen);
    if (rc != EFS_OK)
        return rc;
    if (vlen < EFS_META_DENT_BYTES)
        return EFS_ERR_PROTO;
    memset(out, 0, sizeof(*out));
    out->ino = ((uint64_t)val[0] << 56) | ((uint64_t)val[1] << 48) |
               ((uint64_t)val[2] << 40) | ((uint64_t)val[3] << 32) |
               ((uint64_t)val[4] << 24) | ((uint64_t)val[5] << 16) |
               ((uint64_t)val[6] << 8) | (uint64_t)val[7];
    out->generation = ((uint64_t)val[8] << 56) | ((uint64_t)val[9] << 48) |
                      ((uint64_t)val[10] << 40) | ((uint64_t)val[11] << 32) |
                      ((uint64_t)val[12] << 24) | ((uint64_t)val[13] << 16) |
                      ((uint64_t)val[14] << 8) | (uint64_t)val[15];
    out->type = ((uint32_t)val[16] << 24) | ((uint32_t)val[17] << 16) |
                ((uint32_t)val[18] << 8) | (uint32_t)val[19];
    return EFS_OK;
}

static int read_kv(struct efs_sim *sim, uint32_t shard, struct efs_kv **kv)
{
    int rc = sim_raft_read_group(sim, sim_shard_group(shard));

    if (rc != EFS_OK)
        return rc;
    *kv = sim_raft_kv_group(sim, sim_shard_group(shard));
    return *kv ? EFS_OK : EFS_ERR_BUSY;
}

static void fill_txid(struct efs_sim *sim, struct efs_txid *t)
{
    int i;

    for (i = 0; i < EFS_TXN_ID_LEN; i += 8) {
        uint64_t r = efs_sim_rng(sim);
        memcpy(t->bytes + i, &r, 8);
    }
}

static int mkdir_build(struct efs_sim *sim, efs_ino_t parent, const char *name,
                       struct efs_txid *t, struct efs_txn_parts *p,
                       efs_ino_t *ino_out)
{
    uint32_t psh, csh;
    struct efs_kv *pkv, *ckv;
    struct efs_meta_row prow, crow;
    struct efs_meta_dentry dent;
    struct efs_meta_dentry exist;
    efs_ino_t next = 0, ino;
    uint8_t k_dent[EFS_KV_KEY_MAX], k_pino[EFS_KV_KEY_MAX], k_cino[EFS_KV_KEY_MAX];
    uint8_t k_alloc[EFS_KV_KEY_MAX], k_dseq[EFS_KV_KEY_MAX];
    uint8_t v_dent[EFS_META_DENT_BYTES], v_pino[EFS_META_INO_BYTES];
    uint8_t v_cino[EFS_META_INO_BYTES], v_alloc[EFS_META_ALLOC_BYTES];
    uint8_t v_dseq[8];
    uint32_t kd = 0, kpi = 0, kci = 0, ka = 0, ks = 0;
    uint64_t pver = 0, aver = 0, sver = 0, seq = 0;
    int rc, i;

    psh = efs_kv_inode_shard(parent);
    csh = efs_kv_mkdir_shard(parent, name, 0);
    rc = read_kv(sim, psh, &pkv);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_get_inode(pkv, parent, &prow);
    if (rc != EFS_OK)
        return rc;
    if (!S_ISDIR(prow.mode))
        return EFS_ERR_INVAL;
    rc = efs_kv_key_dentry(psh, parent, name, k_dent, &kd);
    if (rc != EFS_OK)
        return rc;
    {
        struct coord_ctx ctx = { .sim = sim };
        uint8_t tmp[EFS_META_DENT_BYTES];
        uint32_t tn = sizeof(tmp);

        rc = efs_txn_read(pkv, k_dent, kd, sim_coord, &ctx, tmp, &tn);
        if (rc == EFS_OK)
            return EFS_ERR_EXIST;
        if (rc != EFS_ERR_NOT_FOUND)
            return rc;
    }
    (void)exist;
    rc = read_kv(sim, csh, &ckv);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_apply_peek_alloc(ckv, csh, &next);
    if (rc != EFS_OK)
        return rc;
    ino = next;
    if (efs_kv_inode_shard(ino) != csh)
        return EFS_ERR_PROTO;
    next = ino + (efs_ino_t)(1u << EFS_KV_SHARD_BITS);

    memset(&crow, 0, sizeof(crow));
    crow.ino = ino;
    crow.generation = 1;
    crow.mode = S_IFDIR | 0755;
    crow.nlink = 2;
    crow.parent = parent;
    memset(&dent, 0, sizeof(dent));
    dent.ino = ino;
    dent.generation = 1;
    dent.type = S_IFDIR;
    prow.nlink++;
    rc = efs_meta_pack_dentry(&dent, v_dent, sizeof(v_dent));
    if (rc == EFS_OK)
        rc = efs_meta_pack_inode(&prow, v_pino, sizeof(v_pino));
    if (rc == EFS_OK)
        rc = efs_meta_pack_inode(&crow, v_cino, sizeof(v_cino));
    if (rc != EFS_OK)
        return rc;
    wr64(v_alloc, next);

    rc = efs_kv_key_inode(psh, parent, k_pino, &kpi);
    if (rc == EFS_OK)
        rc = efs_kv_key_inode(csh, ino, k_cino, &kci);
    if (rc == EFS_OK)
        rc = efs_kv_key_alloc(csh, k_alloc, &ka);
    if (rc == EFS_OK)
        rc = efs_kv_key_dseq(psh, parent, 0, k_dseq, &ks);
    if (rc != EFS_OK)
        return rc;
    rc = efs_txn_ver_get(pkv, k_pino, kpi, &pver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(ckv, k_alloc, ka, &aver);
    if (rc == EFS_OK)
        rc = efs_txn_ver_get(pkv, k_dseq, ks, &sver);
    if (rc != EFS_OK)
        return rc;
    {
        uint8_t sb[8];
        uint32_t sn = 8;
        int gr = efs_kv_get(pkv, k_dseq, ks, sb, &sn);

        seq = (gr == EFS_OK && sn >= 8) ? rd64(sb) : 0;
    }
    wr64(v_dseq, seq + 1);

    memset(p, 0, sizeof(*p));
    if (psh == csh) {
        p->n = 1;
        p->shard[0] = psh;
    } else {
        p->n = 2;
        if (psh < csh) {
            p->shard[0] = psh;
            p->shard[1] = csh;
        } else {
            p->shard[0] = csh;
            p->shard[1] = psh;
        }
    }
    fill_txid(sim, t);

    /* Namespace txn: canonical shard then key order. */
    for (i = 0; i < p->n; i++) {
        uint32_t sh = p->shard[i];

        if (sh == psh) {
            rc = propose_prep(sim, psh, EFS_TXN_EXCL, t, p, k_dent, kd, 0,
                              EFS_TXN_PUT, v_dent, sizeof(v_dent), NULL);
            if (rc != EFS_OK)
                goto fail;
            rc = propose_prep(sim, psh, EFS_TXN_EXCL, t, p, k_pino, kpi, pver,
                              EFS_TXN_PUT, v_pino, sizeof(v_pino), NULL);
            if (rc != EFS_OK)
                goto fail;
            rc = propose_prep(sim, psh, EFS_TXN_EXCL, t, p, k_dseq, ks, sver,
                              EFS_TXN_PUT, v_dseq, 8, NULL);
            if (rc != EFS_OK)
                goto fail;
        }
        if (sh == csh) {
            rc = propose_prep(sim, csh, EFS_TXN_EXCL, t, p, k_cino, kci, 0,
                              EFS_TXN_PUT, v_cino, sizeof(v_cino), NULL);
            if (rc != EFS_OK)
                goto fail;
            rc = propose_prep(sim, csh, EFS_TXN_EXCL, t, p, k_alloc, ka, aver,
                              EFS_TXN_PUT, v_alloc, sizeof(v_alloc), NULL);
            if (rc != EFS_OK)
                goto fail;
        }
    }
    if (ino_out)
        *ino_out = ino;
    return EFS_OK;
fail:
    (void)propose_drop(sim, psh, t);
    if (csh != psh)
        (void)propose_drop(sim, csh, t);
    return rc;
}

int sim_txn_mkdir_until(struct efs_sim *sim, int client, efs_ino_t parent,
                        const char *name, struct efs_txid *txid, efs_ino_t *out,
                        int until)
{
    struct efs_txid t;
    struct efs_txn_parts p;
    efs_ino_t ino = 0;
    uint32_t coord, psh, csh;
    int rc, i;

    if (!sim || !name || parent == 0)
        return EFS_ERR_INVAL;
    if (client < 0 || client >= sim->nclients)
        client = 0;
    psh = efs_kv_inode_shard(parent);
    csh = efs_kv_mkdir_shard(parent, name, 0);
    rc = sim_sess_ensure(sim, client, psh);
    if (rc != EFS_OK)
        return rc;
    if (csh != psh) {
        rc = sim_sess_ensure(sim, client, csh);
        if (rc != EFS_OK)
            return rc;
    }
    rc = mkdir_build(sim, parent, name, &t, &p, &ino);
    if (rc != EFS_OK)
        return rc;
    sim->txn_id = t;
    sim->txn_parts = p;
    sim->txn_live = 1;
    if (txid)
        *txid = t;
    if (out)
        *out = ino;
    sim->last_ino = ino;
    if (until <= TXN_PREPARE)
        return EFS_OK;
    coord = efs_txn_coordinator(&t, &p);
    rc = propose_decide(sim, coord, &t, EFS_TXN_COMMIT);
    if (rc != EFS_OK)
        return rc;
    if (until <= TXN_DECISION)
        return EFS_OK;
    for (i = 0; i < p.n; i++) {
        rc = propose_resolve(sim, p.shard[i], &t, EFS_TXN_COMMIT);
        if (rc != EFS_OK)
            return rc;
    }
    sim->txn_live = 0;
    return EFS_OK;
}

int sim_txn_finish(struct efs_sim *sim, const struct efs_txid *t, int commit)
{
    uint32_t coord;
    int rc, i, dec;
    const struct efs_txid *id;
    struct efs_txn_parts p;

    if (!sim || !sim->txn_live)
        return EFS_ERR_INVAL;
    id = t ? t : &sim->txn_id;
    p = sim->txn_parts;
    coord = efs_txn_coordinator(id, &p);
    dec = commit ? EFS_TXN_COMMIT : EFS_TXN_ABORT;
    rc = propose_decide(sim, coord, id, dec);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < p.n; i++) {
        rc = propose_resolve(sim, p.shard[i], id, dec);
        if (rc != EFS_OK)
            return rc;
    }
    sim->txn_live = 0;
    return EFS_OK;
}

int efs_sim_mkdir(struct efs_sim *sim, int client, efs_ino_t parent,
                  const char *name, efs_ino_t *out)
{
    int rc;

    rc = sim_txn_mkdir_until(sim, client, parent, name, NULL, out, TXN_RESOLVE);
    if (out && rc != EFS_OK)
        *out = 0;
    return rc;
}

int efs_sim_mkdir_until(struct efs_sim *sim, efs_ino_t parent, const char *name,
                        struct efs_txid *txid, efs_ino_t *out, int until)
{
    return sim_txn_mkdir_until(sim, 0, parent, name, txid, out, until);
}

int efs_sim_txn_finish(struct efs_sim *sim, const struct efs_txid *t, int commit)
{
    return sim_txn_finish(sim, t, commit);
}
