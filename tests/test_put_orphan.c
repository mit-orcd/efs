/* D31 (K13) collection of abandoned ticketed uploads, in process: the real
 * ticket ledger, session state machine and collector step on a durable LSM
 * KV (reopened between passes, as a new leader would find it), with three
 * members modelled on in-memory fragment stores. The member model follows
 * server_put_orphan_delete's order (own fragment, storage floor, group
 * authority, checksum-guarded delete); tests/test_put_orphan_server.c covers
 * the real member code. */
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/put_orphan.h"
#include "efs/session.h"
#include "efs/store.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_tmpdir.h"

#define MEMBERS 3
struct cluster {
    struct efs_kv *kv;
    struct efs_store *store[MEMBERS];
    uint32_t floor[MEMBERS];       /* storage session floor per member */
    int down[MEMBERS];
    int drop_ack_before, drop_ack_after; /* lost ACK before/after commit */
    unsigned deletes, acks;
};
static struct cluster c;

static struct efs_meta_pub make_pub(efs_ino_t ino, uint64_t gen, uint64_t seq)
{
    struct efs_meta_pub p = {0};
    p.ino = ino;
    p.inode_gen = gen;
    p.new_size = 64;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    p.fresh_object = 1;
    p.ticketed = 1;
    p.put_fragment_len = 32;
    p.put_member_mask = 7;
    p.put_id.client_uuid[0] = 77;
    p.put_id.session_epoch = 1;
    p.put_id.seq = seq;
    p.publication_id = p.put_id;
    p.candidate_gen = efs_meta_candidate_gen(p.put_id.client_uuid, 1, seq, 0, 0);
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        p.ch.nodes[i] = i + 1;
        memset(p.ch.checksums[i], (int)(16 * seq + i + 1), EFS_HASH_SIZE);
    }
    return p;
}
static struct efs_frag_id fid(const struct efs_put_ticket *t, unsigned fi)
{
    return (struct efs_frag_id){.export_id = 1, .ino = t->ino, .inode_generation = t->inode_gen,
                                .chunk_generation = t->object, .chunk_index = t->ci,
                                .fragment_index = fi, .coding_profile_id = t->coding_profile};
}
/* The PUT path stored each fragment on nodes[fi] while ADMITTED. */
static void land(const struct efs_put_ticket *t)
{
    uint8_t data[32];
    for (unsigned fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        struct efs_frag_id id = fid(t, fi);
        memset(data, (int)fi, sizeof(data));
        assert(efs_store_put(c.store[t->nodes[fi] - 1], &id, data, sizeof(data),
                             t->checksums[fi]) == EFS_OK);
    }
}
static unsigned present(const struct efs_put_ticket *t)
{
    unsigned mask = 0;
    for (unsigned fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        struct efs_frag_id id = fid(t, fi);
        uint8_t buf[64], sum[EFS_HASH_SIZE];
        uint32_t len = sizeof(buf);
        int ok = 0;
        if (efs_store_get(c.store[t->nodes[fi] - 1], &id, buf, &len, sum, &ok) == EFS_OK)
            mask |= 1u << fi;
    }
    return mask;
}
static uint32_t lane_shard(const struct efs_put_ticket *t)
{
    return efs_kv_lane_shard(t->ino, (uint8_t)(t->ci % EFS_META_LANES));
}

/* ---- the collector's three effects ---- */
static int revoked(void *ctx, const uint8_t uuid[EFS_OPID_UUID_LEN], uint32_t epoch)
{
    (void)ctx;
    return efs_session_barrier_done(c.kv, uuid, epoch);
}
static int member_delete(void *ctx, const struct efs_put_ticket *t, unsigned fi)
{
    (void)ctx;
    unsigned m = t->nodes[fi] - 1;
    if (c.down[m])
        return EFS_ERR_IO;
    if (c.floor[m] <= t->id.session_epoch)
        return EFS_ERR_BUSY;
    int rc = efs_session_local_revoked(c.kv, lane_shard(t), t->id.client_uuid,
                                       t->id.session_epoch);
    if (rc == EFS_OK)
        rc = efs_put_ticket_orphan_get(c.kv, t, NULL);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_ERR_STALE;
    if (rc != EFS_OK)
        return rc;
    struct efs_frag_id id = fid(t, fi);
    rc = efs_store_del_if_sum(c.store[m], &id, t->checksums[fi]);
    c.deletes++;
    return rc == EFS_ERR_EXIST ? EFS_ERR_PROTO : rc;
}
static int ack(void *ctx, const struct efs_put_ticket *t, uint32_t frags)
{
    (void)ctx;
    if (c.drop_ack_before) {
        c.drop_ack_before--;
        return EFS_ERR_IO; /* proposal lost before commit */
    }
    int rc = efs_put_ticket_orphan_ack(c.kv, t, frags);
    c.acks++;
    if (c.drop_ack_after) {
        c.drop_ack_after--;
        return EFS_ERR_IO; /* committed, reply lost */
    }
    return rc;
}
static const struct efs_put_orphan_ops ops = {NULL, revoked, member_delete, ack};

/* One leader pass over the whole ledger (both anchors), from KV state only. */
struct pass_scan {
    struct efs_put_ticket t[16];
    uint32_t acked[16], n;
};
static int pass_cb(void *user, const uint8_t *key, uint32_t kl, const uint8_t *val, uint32_t vl)
{
    struct pass_scan *s = user;
    assert(s->n < 16);
    assert(efs_put_ticket_orphan_decode(key, kl, val, vl, &s->t[s->n], &s->acked[s->n]) == EFS_OK);
    s->n++;
    return 0;
}
static unsigned pass(struct efs_put_orphan_stats *st)
{
    struct pass_scan s = {0};
    for (uint32_t anchor = 1; anchor <= 2; anchor++) {
        uint8_t prefix[3];
        efs_put_ticket_orphan_prefix(anchor, prefix);
        int rc = efs_kv_scan_prefix(c.kv, prefix, sizeof(prefix), pass_cb, &s);
        assert(rc == EFS_OK);
    }
    uint64_t skip = 0;
    for (uint32_t i = 0; i < s.n; i++) {
        int rc = efs_put_orphan_collect(c.kv, &s.t[i], s.acked[i], &ops, &skip, st);
        assert(rc == EFS_OK || rc == EFS_ERR_BUSY || rc == EFS_ERR_IO);
    }
    return s.n;
}
static uint32_t acked_of(const struct efs_put_ticket *t)
{
    uint32_t a = 99;
    assert(efs_put_ticket_orphan_get(c.kv, t, &a) == EFS_OK);
    return a;
}

static void reopen(const char *dir)
{
    efs_kv_lsm_close(c.kv);
    c.kv = efs_kv_lsm_open(dir, NULL);
    assert(c.kv);
}
static void rm_tree(const char *dir)
{
    DIR *dp = opendir(dir);
    assert(dp);
    struct dirent *e;
    char path[EFS_TEST_PATH + 300];
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        assert(!lstat(path, &st));
        if (S_ISDIR(st.st_mode))
            rm_tree(path);
        else
            assert(!unlink(path));
    }
    closedir(dp);
    assert(!rmdir(dir));
}

struct cap { uint8_t *k, *v; };
static int grab(void *u, const uint8_t *key, uint32_t kl, const uint8_t *val, uint32_t vl)
{
    struct cap *x = u;
    assert(kl == EFS_PUT_ORPHAN_KEY_LEN && vl == EFS_PUT_TICKET_VAL_LEN);
    memcpy(x->k, key, kl);
    memcpy(x->v, val, vl);
    return 0;
}
static void codec_and_bounds(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    assert(kv);
    struct efs_meta_attrs a = {0, 0, 1};
    efs_ino_t ino;
    struct efs_meta_row row;
    assert(efs_meta_apply_init(kv, 1) == EFS_OK);
    assert(efs_meta_apply_create_file(kv, &a, EFS_ROOT_INO, S_IFREG | 0600, "x", &ino) == EFS_OK);
    assert(efs_meta_apply_get_inode(kv, ino, &row) == EFS_OK);
    struct efs_meta_pub p = make_pub(ino, row.generation, 1);
    struct efs_put_ticket t, back;
    assert(efs_put_ticket_from_pub(&p, &t) == EFS_OK);
    uint32_t sh = lane_shard(&t);
    assert(efs_session_create(kv, t.id.client_uuid, 1) == EFS_OK);
    assert(efs_session_register(kv, t.id.client_uuid, 1, sh) == EFS_OK);
    assert(efs_session_establish(kv, sh, t.id.client_uuid, 1) == EFS_OK);
    assert(efs_put_ticket_admit(kv, &t) == EFS_OK);
    assert(efs_put_ticket_settle(kv, &t) == EFS_OK);
    /* The record lives on the group's anchor shard, not the lane shard. */
    uint8_t prefix[3];
    efs_put_ticket_orphan_prefix(efs_kv_anchor_shard(sh), prefix);
    struct pass_scan s = {0};
    assert(efs_kv_scan_prefix(kv, prefix, 3, pass_cb, &s) == EFS_OK && s.n == 1);
    back = s.t[0];
    assert(!memcmp(&back, &t, sizeof(t)) && s.acked[0] == 0);
    /* A record that does not derive from its own key is refused. (Stored
     * checksums cannot be cross-checked here; the member's checksum guard
     * refuses a delete whose bytes do not match.) */
    struct { uint8_t k[EFS_PUT_ORPHAN_KEY_LEN], v[EFS_PUT_TICKET_VAL_LEN]; } rec;
    struct efs_put_ticket out;
    uint32_t acked;
    struct cap cp = {rec.k, rec.v};
    assert(efs_kv_scan_prefix(kv, prefix, 3, grab, &cp) == EFS_OK);
    assert(efs_put_ticket_orphan_decode(rec.k, sizeof(rec.k), rec.v, sizeof(rec.v), &out, &acked) == EFS_OK);
    rec.v[15] ^= 1; /* the object: no longer the ticket's derived name */
    assert(efs_put_ticket_orphan_decode(rec.k, sizeof(rec.k), rec.v, sizeof(rec.v), &out, &acked) == EFS_ERR_PROTO);
    rec.v[15] ^= 1;
    rec.v[7] = EFS_PUT_TICKET_ADMITTED; /* not a REJECTED body */
    assert(efs_put_ticket_orphan_decode(rec.k, sizeof(rec.k), rec.v, sizeof(rec.v), &out, &acked) == EFS_ERR_PROTO);
    rec.v[7] = EFS_PUT_TICKET_REJECTED;
    rec.k[1] ^= 3; /* wrong anchor */
    assert(efs_put_ticket_orphan_decode(rec.k, sizeof(rec.k), rec.v, sizeof(rec.v), &out, &acked) == EFS_ERR_PROTO);
    rec.k[1] ^= 3;
    rec.v[179] = 7; /* a fully acknowledged record is never stored */
    assert(efs_put_ticket_orphan_decode(rec.k, sizeof(rec.k), rec.v, sizeof(rec.v), &out, &acked) == EFS_ERR_PROTO);
    assert(efs_put_ticket_orphan_ack(kv, &t, 0) == EFS_ERR_INVAL);
    assert(efs_put_ticket_orphan_ack(kv, &t, 8) == EFS_ERR_INVAL);
    efs_kv_mem_free(kv);
}

int main(void)
{
    codec_and_bounds();
    char dir[EFS_TEST_PATH];
    efs_test_path(dir, sizeof(dir), "efs-put-orphan-XXXXXX");
    assert(efs_test_mkdtemp(dir));
    c.kv = efs_kv_lsm_open(dir, NULL);
    assert(c.kv);
    for (unsigned m = 0; m < MEMBERS; m++)
        assert((c.store[m] = efs_store_mem_create()));

    struct efs_meta_attrs attrs = {0, 0, 1};
    efs_ino_t ino;
    struct efs_meta_row row;
    assert(efs_meta_apply_init(c.kv, 1) == EFS_OK);
    assert(efs_meta_apply_create_file(c.kv, &attrs, EFS_ROOT_INO, S_IFREG | 0600, "abandon", &ino) == EFS_OK);
    assert(efs_meta_apply_get_inode(c.kv, ino, &row) == EFS_OK);
    struct efs_meta_pub pp = make_pub(ino, row.generation, 1), ap = make_pub(ino, row.generation, 2),
                        bp = make_pub(ino, row.generation, 3), dp = make_pub(ino, row.generation, 4);
    struct efs_put_ticket pub, a, b, d;
    assert(efs_put_ticket_from_pub(&pp, &pub) == EFS_OK && efs_put_ticket_from_pub(&ap, &a) == EFS_OK &&
           efs_put_ticket_from_pub(&bp, &b) == EFS_OK && efs_put_ticket_from_pub(&dp, &d) == EFS_OK);
    const uint8_t *u = pub.id.client_uuid;
    uint32_t sh = lane_shard(&pub);
    assert(efs_session_create(c.kv, u, 1) == EFS_OK);
    assert(efs_session_register(c.kv, u, 1, sh) == EFS_OK);
    assert(efs_session_establish(c.kv, sh, u, 1) == EFS_OK);
    /* pub is published; a, b, d are admitted, stored, then abandoned by
     * their owner's SETTLE. d's fragment 0 on member 1 is another body. */
    struct efs_put_ticket *all[4] = {&pub, &a, &b, &d};
    for (unsigned i = 0; i < 4; i++) {
        assert(efs_put_ticket_admit(c.kv, all[i]) == EFS_OK);
        land(all[i]);
    }
    {
        struct efs_frag_id id = fid(&d, 0);
        uint8_t other[32] = {9}, sum[EFS_HASH_SIZE];
        memset(sum, 0xEE, sizeof(sum));
        assert(efs_store_del(c.store[0], &id) == EFS_OK);
        assert(efs_store_put(c.store[0], &id, other, sizeof(other), sum) == EFS_OK);
    }
    assert(efs_meta_apply_publish(c.kv, &pp) == EFS_OK);
    for (unsigned i = 0; i < 4; i++)
        assert(efs_put_ticket_settle(c.kv, all[i]) == EFS_OK);
    assert(efs_put_ticket_orphan(c.kv, &pub) == EFS_ERR_NOT_FOUND);
    assert(acked_of(&a) == 0 && acked_of(&b) == 0 && acked_of(&d) == 0);

    /* Racing re-admission/publication of an abandoned ticket is refused;
     * a late settle is a no-op and the record stays. */
    uint32_t state;
    assert(efs_put_ticket_admit(c.kv, &a) == EFS_ERR_STALE);
    assert(efs_meta_apply_publish(c.kv, &ap) == EFS_ERR_STALE);
    assert(efs_put_ticket_state(c.kv, &a, &state) == EFS_OK && state == EFS_PUT_TICKET_RETIRED);
    assert(efs_put_ticket_settle(c.kv, &a) == EFS_OK && acked_of(&a) == 0);

    /* 1. Safety rule: nothing is deleted before revocation is proven. */
    struct efs_put_orphan_stats st = {0};
    assert(pass(&st) == 3 && st.waiting == 3 && !st.deleted && !c.deletes);
    assert(present(&a) == 7 && present(&b) == 7);
    assert(efs_put_ticket_orphan_ack(c.kv, &a, 1) == EFS_ERR_BUSY); /* no shard floor */
    assert(member_delete(NULL, &a, 0) == EFS_ERR_BUSY);              /* no storage floor */
    /* Revocation in protocol order: BEGIN, storage seal on every member,
     * shard floor, then FINISH. Until FINISH the global barrier is open. */
    assert(efs_session_begin_fence(c.kv, u) == EFS_OK);
    for (unsigned m = 0; m < MEMBERS; m++)
        c.floor[m] = 2;
    assert(efs_session_fence_local(c.kv, sh, u, 2) == EFS_OK);
    memset(&st, 0, sizeof(st));
    assert(pass(&st) == 3 && st.waiting == 3 && !c.deletes && present(&a) == 7);
    /* The shard floor alone lets a member's authority read succeed, but the
     * collector still waits for the global barrier. */
    assert(efs_session_ack_fence(c.kv, u, sh) == EFS_OK);
    assert(efs_session_finish_fence(c.kv, u) == EFS_OK);
    /* Re-admission after revocation is fenced, not merely stale. */
    assert(efs_put_ticket_admit(c.kv, &a) == EFS_ERR_FENCED);

    /* 2. A member down: deletes on the others are acknowledged; the record
     * is retained and never declared collected. */
    c.down[2] = 1;
    memset(&st, 0, sizeof(st));
    assert(pass(&st) == 3);
    assert(!st.collected && st.deleted == 5 && st.failures >= 2);
    assert(acked_of(&a) == 3 && acked_of(&b) == 3 && acked_of(&d) == 2);
    assert(present(&a) == 4 && present(&b) == 4 && present(&d) == 5);
    /* Restart/leader change: progress is only the durable acknowledged mask. */
    reopen(dir);
    for (unsigned i = 0; i < 5; i++) {
        memset(&st, 0, sizeof(st));
        assert(pass(&st) == 3 && !st.collected);
    }
    assert(acked_of(&a) == 3 && present(&a) == 4);

    /* 3. Lost acknowledgements: member 3 returns. a's ACK is lost before
     * commit, b's after commit. Neither double-counts nor loses a record. */
    c.down[2] = 0;
    c.drop_ack_before = 1;
    memset(&st, 0, sizeof(st));
    assert(pass(&st) == 3);
    assert(acked_of(&a) == 3 && present(&a) == 0); /* deleted, not acknowledged */
    assert(efs_put_ticket_orphan(c.kv, &b) == EFS_ERR_NOT_FOUND && present(&b) == 0);
    assert(st.collected == 1);
    reopen(dir);
    c.drop_ack_after = 1;
    memset(&st, 0, sizeof(st));
    assert(pass(&st) == 2); /* a and d remain */
    assert(efs_put_ticket_orphan(c.kv, &a) == EFS_ERR_NOT_FOUND);
    /* The lost reply is resolved by the record: a later duplicate ACK is a
     * no-op on a retired orphan and deletes nothing. */
    assert(efs_put_ticket_orphan_ack(c.kv, &a, 4) == EFS_OK);
    assert(efs_put_ticket_orphan_ack(c.kv, &a, 7) == EFS_OK);
    memset(&st, 0, sizeof(st));
    assert(pass(&st) == 1); /* only d */

    /* 4. Checksum guard: d's fragment 0 on member 1 is another body. It is
     * neither deleted nor acknowledged, ever; the record stays pending. */
    for (unsigned i = 0; i < 3; i++) {
        memset(&st, 0, sizeof(st));
        assert(pass(&st) == 1 && !st.collected && st.failures == 1);
    }
    assert(acked_of(&d) == 6 && present(&d) == 1);

    /* 5. Negative control: the published body is never deleted. It has no
     * orphan record; a member asked directly refuses; an ACK for it is a
     * no-op on its retired ticket. */
    assert(present(&pub) == 7);
    for (unsigned fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
        assert(member_delete(NULL, &pub, fi) == EFS_ERR_STALE);
    assert(efs_put_ticket_orphan_ack(c.kv, &pub, 7) == EFS_OK);
    assert(present(&pub) == 7);
    struct efs_meta_chunk ch;
    assert(efs_meta_apply_get_chunk(c.kv, ino, 0, &ch) == EFS_OK && ch.generation == pub.object);
    /* A live (never settled) ticket has no record either. */
    struct efs_meta_pub lp = make_pub(ino, row.generation, 5);
    struct efs_put_ticket live;
    assert(efs_put_ticket_from_pub(&lp, &live) == EFS_OK);
    assert(efs_put_ticket_orphan_ack(c.kv, &live, 1) == EFS_ERR_NOT_FOUND);
    assert(member_delete(NULL, &live, 0) == EFS_ERR_STALE);

    efs_kv_lsm_close(c.kv);
    for (unsigned m = 0; m < MEMBERS; m++)
        efs_store_mem_free(c.store[m]);
    rm_tree(dir);
    puts("D31 abandoned-upload collection: refused before revocation (storage, shard and global floors), "
         "per-member ACK with a member down retained across restart, lost ACK before/after commit, "
         "re-admission refused, checksum guard keeps a foreign body, published body never deleted PASS");
    return 0;
}
