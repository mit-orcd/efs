/* W125 background packer.
 *
 * Runs on every daemon; works only for the metadata groups this node leads
 * (at most one packer per group). Each pass of a group:
 *   1. abandon: cancel ACTIVE segments with PENDING records that this
 *      packer (this node, this term) does not own; a crashed or deposed
 *      packer's copies are then reclaimed by the retirement pass;
 *   2. select: walk the group's candidate index (creation epoch order) and
 *      re-validate each file on the local replica: same generation, linked,
 *      regular, at most EFS_PACK_THRESHOLD bytes, only chunk zero, a plain
 *      per-file base without spans, quiet for the minimum age, no lease.
 *      Files that can never qualify (gone, grown, already packed) are
 *      unindexed; files not yet quiet are left for a later pass;
 *   3. group: eligible files sorted by (epoch, parent directory, inode)
 *      fill segments up to the record and byte caps, so files that die
 *      together share a segment;
 *   4. copy: read each file's bytes (data fragment 0, or fragments 1 and 2
 *      XORed), build and encode the segment, ADMIT it, PUT its fragments to
 *      all members (each fsynced before acknowledging), then SWITCH the
 *      files with the exact-source CAS; a failed copy is CANCELLED;
 *   5. unindex the switched and the refused files.
 * The switch, not this code, is the authority: any file that changed after
 * selection is cancelled there and stays a candidate.
 *
 * Tunables (environment, read at start): EFS_PACK_MIN_AGE_MS (default
 * 60000), EFS_PACK_INTERVAL_MS (1000), EFS_PACK_SEGMENTS_PER_PASS (8). None
 * of them turns the packer off: values below 1 are raised to 1 (the minimum
 * age may be 0 for tests). */
#include "server_internal.h"
#include "efs/checksum.h"
#include "efs/erasure.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include "efs/pack_mapping.h"
#include "efs/raft.h"
#include "efs/session.h"
#include "efs/store.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PK_SCAN_MAX 4096u

static struct {
    struct efsd_server *s;
    pthread_t tid;
    int started, stop;
    uint64_t min_age_ns, interval_ms;
    uint32_t segments_per_pass;
    struct efs_packer_status st;
    pthread_mutex_t mu;
} pk = {.mu = PTHREAD_MUTEX_INITIALIZER};

static uint64_t pk_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t env_u64(const char *name, uint64_t def, uint64_t min)
{
    const char *v = getenv(name);
    if (!v || !*v) return def;
    char *end;
    unsigned long long x = strtoull(v, &end, 10);
    if (*end) return def;
    return x < min ? min : (uint64_t)x;
}
static void wr64(uint8_t *p, uint64_t v) { for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }
#define STAT_ADD(f, n) __atomic_fetch_add(&pk.st.f, (n), __ATOMIC_RELAXED)

void server_packer_status(struct efs_packer_status *out)
{
    pthread_mutex_lock(&pk.mu);
    *out = pk.st;
    pthread_mutex_unlock(&pk.mu);
}

/* The packer's owner identity for a group under a term. */
static void pk_owner(uint8_t group, uint64_t term, struct efs_opid *o)
{
    memset(o, 0, sizeof(*o));
    memcpy(o->client_uuid, "EFSPACK", 7);
    o->client_uuid[8] = group;
    o->client_uuid[9] = (uint8_t)(pk.s->id >> 8);
    o->client_uuid[10] = (uint8_t)pk.s->id;
    o->client_uuid[15] = 1;
    o->session_epoch = (uint32_t)(term ? term : 1);
    o->seq = 1;
}

/* ---- 1. abandon foreign pending segments ---- */
struct pk_abandon { struct efs_opid me; uint64_t ids[64], gens[64]; uint32_t n; struct efs_pack_object o; };
static int pk_abandon_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    struct pk_abandon *a = u;
    if (kl != EFS_PACK_OBJECT_KEY_BYTES || k[2] != EFS_KV_KIND_PACK_SEGMENT) return 0;
    if (efs_pack_object_decode(v, vl, &a->o) != EFS_OK || a->o.ledger.phase != EFS_PACK_ACTIVE) return 0;
    struct efs_pack_ledger_usage use;
    efs_pack_ledger_usage(&a->o.ledger, &use);
    if (!use.pending) return 0;
    if (!memcmp(a->o.ledger.owner.client_uuid, a->me.client_uuid, EFS_OPID_UUID_LEN) &&
        a->o.ledger.owner.session_epoch == a->me.session_epoch) return 0; /* ours, in flight */
    a->ids[a->n] = a->o.ledger.segment_id;
    a->gens[a->n] = a->o.ledger.segment_generation;
    return ++a->n == 64;
}
static void pk_cancel(uint8_t group, uint64_t id, uint64_t gen)
{
    uint8_t cmd[18];
    cmd[0] = EFS_MD_CMD_PACK_CANCEL; cmd[1] = group;
    wr64(cmd + 2, id); wr64(cmd + 10, gen);
    (void)server_raft_host_pack_propose(group, cmd, sizeof(cmd));
}
static void pk_abandon(struct efs_kv *kv, uint8_t group, uint64_t term)
{
    struct pk_abandon *a = calloc(1, sizeof(*a));
    if (!a) return;
    pk_owner(group, term, &a->me);
    uint32_t anchor = efs_pack_group_anchor(group);
    uint8_t prefix[3] = {(uint8_t)(anchor >> 8), (uint8_t)anchor, EFS_KV_KIND_PACK_SEGMENT};
    (void)efs_kv_scan_prefix(kv, prefix, 3, pk_abandon_cb, a);
    for (uint32_t i = 0; i < a->n; i++) { pk_cancel(group, a->ids[i], a->gens[i]); STAT_ADD(abandoned, 1); }
    free(a);
}

/* ---- 2. select ---- */
struct pk_entry { uint64_t epoch; efs_ino_t ino; uint64_t gen; };
struct pk_scan { struct pk_entry *e; uint32_t n; };
static int pk_scan_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    struct pk_scan *sc = u;
    if (kl != EFS_PACK_CANDIDATE_KEY_BYTES || k[2] != EFS_KV_KIND_PACK_CANDIDATE || vl != 8) return 0;
    sc->e[sc->n++] = (struct pk_entry){rd64(k + 3), rd64(k + 11), rd64(v)};
    return sc->n == PK_SCAN_MAX;
}
struct pk_file {
    struct efs_pack_candidate c;
    uint64_t epoch;
    uint8_t *data;
};
static int pk_file_cmp(const void *a, const void *b)
{
    const struct pk_file *x = a, *y = b;
    if (x->epoch != y->epoch) return x->epoch < y->epoch ? -1 : 1;
    if (x->c.parent != y->c.parent) return x->c.parent < y->c.parent ? -1 : 1;
    return x->c.ino < y->c.ino ? -1 : x->c.ino > y->c.ino;
}

/* ---- 4. copy ---- */
static const struct efs_node *pk_node(const struct efs_node *nodes, uint32_t count, uint32_t id)
{
    for (uint32_t i = 0; i < count; i++) if (nodes[i].id == id) return &nodes[i];
    return NULL;
}
/* One fragment of a per-file chunk, checksum-verified: local store or a
 * GET_CHUNK to its node. */
struct pk_object { const efs_node_id_t *nodes; const uint8_t (*sums)[EFS_HASH_SIZE]; uint64_t gen; };
static int pk_get_fragment(const struct efs_node *nodes, uint32_t count, const struct efs_pack_candidate *c,
                           const struct pk_object *obj, uint32_t fi, uint8_t *out, uint32_t len)
{
    uint32_t id = obj->nodes[fi];
    uint8_t sum[EFS_HASH_SIZE];
    if (id == pk.s->id) {
        pthread_mutex_lock(&pk.s->lock);
        struct efs_export *ex = server_export_acquire_or_create_locked(pk.s, 1);
        pthread_mutex_unlock(&pk.s->lock);
        if (!ex) return EFS_ERR_IO;
        struct efs_store st;
        struct efs_nvme_store nctx;
        struct efs_frag_id fid = {.export_id = ex->id, .ino = c->ino, .chunk_index = 0,
                                  .fragment_index = fi, .chunk_generation = obj->gen};
        uint32_t got = len;
        int sum_ok = 0;
        efs_store_nvme_bind(&st, &nctx, pk.s, ex);
        int rc = efs_store_get(&st, &fid, out, &got, sum, &sum_ok);
        server_export_put(pk.s, ex);
        if (rc || got != len || !sum_ok) return EFS_ERR_IO;
    } else {
        const struct efs_node *n = pk_node(nodes, count, id);
        if (!n) return EFS_ERR_IO;
        struct efs_conn *pc = server_peer_conn_get(n->addr, n->port);
        if (!pc) return EFS_ERR_NET;
        struct efs_msg_get_chunk req = {.export_id = 1, .ino = c->ino, .chunk_index = 0,
                                        .fragment_index = fi, .chunk_generation = obj->gen};
        uint8_t type;
        void *reply = NULL;
        uint32_t rlen = 0;
        if (efs_conn_send_msg(pc, EFS_MSG_GET_CHUNK, &req, sizeof(req)) ||
            efs_conn_recv_msg(pc, &type, &reply, &rlen)) {
            server_peer_conn_drop(n->addr, n->port, pc);
            free(reply);
            return EFS_ERR_NET;
        }
        server_peer_conn_release(n->addr, n->port, pc);
        const uint8_t *r = reply;
        int ok = type == EFS_MSG_GET_CHUNK_REPLY && rlen == 1 + EFS_HASH_SIZE + len && r[0] == EFS_GET_CHUNK_OK;
        if (ok) { memcpy(sum, r + 1, EFS_HASH_SIZE); memcpy(out, r + 1 + EFS_HASH_SIZE, len); }
        free(reply);
        if (!ok) return EFS_ERR_IO;
    }
    uint8_t h[EFS_HASH_SIZE];
    efs_hash(out, len, h);
    return memcmp(h, obj->sums[fi], EFS_HASH_SIZE) ? EFS_ERR_CHECKSUM : EFS_OK;
}
/* Data fragment 0 (the chunk's first half) of one chunk generation, or
 * fragments 1 and 2 XORed. */
static int pk_get_d1(const struct efs_node *nodes, uint32_t count, const struct efs_pack_candidate *c,
                     const struct pk_object *obj, uint8_t *a, uint8_t *b, uint32_t flen)
{
    if (pk_get_fragment(nodes, count, c, obj, 0, a, flen) == EFS_OK) return EFS_OK;
    int rc = pk_get_fragment(nodes, count, c, obj, 1, a, flen);
    if (rc == EFS_OK) rc = pk_get_fragment(nodes, count, c, obj, 2, b, flen);
    if (rc == EFS_OK) for (uint32_t i = 0; i < flen; i++) a[i] ^= b[i];
    return rc;
}
/* A small file lies wholly in data fragment 0. Its bytes are the base
 * image (absent when the base generation is 0) overlaid by each span's
 * published range, in order, exactly as a client read assembles them. */
static int pk_read_file(const struct efs_node *nodes, uint32_t count, struct pk_file *f, uint32_t flen)
{
    uint8_t *a = malloc(flen), *b = malloc(flen);
    int rc = a && b ? EFS_OK : EFS_ERR_NOMEM;
    if (rc == EFS_OK && f->c.size > flen) rc = EFS_ERR_INVAL;
    if (rc == EFS_OK) {
        f->data = calloc(1, f->c.size);
        if (!f->data) rc = EFS_ERR_NOMEM;
    }
    if (rc == EFS_OK && f->c.base.generation) {
        struct pk_object o = {f->c.base.nodes, (const uint8_t (*)[EFS_HASH_SIZE])f->c.base.checksums,
                              f->c.base.generation};
        rc = pk_get_d1(nodes, count, &f->c, &o, a, b, flen);
        if (rc == EFS_OK) memcpy(f->data, a, f->c.size);
    }
    for (uint32_t i = 0; rc == EFS_OK && i < f->c.ndelta; i++) {
        const struct efs_meta_delta *d = &f->c.deltas[i];
        if (!d->len || !d->generation) continue;
        if (d->off >= f->c.size) continue;
        struct pk_object o = {d->nodes, (const uint8_t (*)[EFS_HASH_SIZE])d->checksums, d->generation};
        rc = pk_get_d1(nodes, count, &f->c, &o, a, b, flen);
        uint64_t end = (uint64_t)d->off + d->len;
        if (end > f->c.size) end = f->c.size;
        if (rc == EFS_OK) memcpy(f->data + d->off, a + d->off, (size_t)(end - d->off));
    }
    free(a);
    free(b);
    return rc;
}
static int pk_put_fragment(const struct efs_node *nodes, uint32_t count, uint32_t id,
                           const uint8_t *body, uint32_t len)
{
    if (id == pk.s->id) return server_segment_put(pk.s, body, len);
    const struct efs_node *n = pk_node(nodes, count, id);
    if (!n) return EFS_ERR_IO;
    struct efs_conn *pc = server_peer_conn_get(n->addr, n->port);
    if (!pc) return EFS_ERR_NET;
    uint8_t type;
    void *reply = NULL;
    uint32_t rlen = 0;
    if (efs_conn_send_msg(pc, EFS_MSG_PUT_SEGMENT, body, len) ||
        efs_conn_recv_msg(pc, &type, &reply, &rlen)) {
        server_peer_conn_drop(n->addr, n->port, pc);
        free(reply);
        return EFS_ERR_NET;
    }
    server_peer_conn_release(n->addr, n->port, pc);
    int rc = type == EFS_MSG_PUT_SEGMENT_REPLY && rlen == sizeof(struct efs_msg_gc_inode_reply)
             ? ((struct efs_msg_gc_inode_reply *)reply)->rc : EFS_ERR_PROTO;
    free(reply);
    return rc;
}
static void pk_unindex(uint8_t group, const struct pk_entry *e, uint32_t n)
{
    while (n) {
        uint32_t m = n < EFS_PACK_UNINDEX_MAX ? n : EFS_PACK_UNINDEX_MAX;
        uint8_t *cmd = malloc(4u + 16u * m);
        if (!cmd) return;
        cmd[0] = EFS_MD_CMD_PACK_UNINDEX; cmd[1] = group;
        cmd[2] = (uint8_t)(m >> 8); cmd[3] = (uint8_t)m;
        for (uint32_t i = 0; i < m; i++) { wr64(cmd + 4 + 16 * i, e[i].epoch); wr64(cmd + 12 + 16 * i, e[i].ino); }
        if (server_raft_host_pack_propose(group, cmd, 4u + 16u * m) == EFS_OK) STAT_ADD(unindexed, m);
        free(cmd);
        e += m;
        n -= m;
    }
}

/* Pack files[0..n) (already read) into one segment. Returns the number of
 * files switched; *done receives the index entries to drop. */
static uint32_t pk_segment(uint8_t group, uint64_t term, const struct efs_node *nodes, uint32_t count,
                           struct pk_file *files, uint32_t n, struct pk_entry *done, uint32_t *ndone)
{
    uint32_t live_total = 0;
    struct efs_pack_record *rec = calloc(n, sizeof(*rec));
    struct efs_pack_ref *refs = calloc(n, sizeof(*refs));
    uint8_t *image = malloc(EFS_SEGMENT_MAX_SIZE);
    struct efs_pack_object *o = calloc(1, sizeof(*o));
    uint8_t *frag = malloc(EFS_SEGMENT_MAX_SIZE / 2 * 3);
    uint8_t *body = malloc(EFS_SEGMENT_PUT_HEADER + EFS_SEGMENT_MAX_SIZE / 2);
    uint8_t *cmd = malloc(2u + EFS_PACK_OBJECT_MAX_BYTES);
    struct efs_pack_switch *sw = calloc(1, sizeof(*sw));
    uint8_t *swire = malloc(1u + 19u + EFS_PACK_SWITCH_MAX * EFS_MD_PACK_SWITCH_MEMBER_BYTES);
    if (!rec || !refs || !image || !o || !frag || !body || !cmd || !sw || !swire) goto out;
    uint64_t id = (pk_now_ns() << 8) | (pk.s->id & 0xff), gen = 1;
    for (uint32_t i = 0; i < n; i++)
        rec[i] = (struct efs_pack_record){.ino = files[i].c.ino, .inode_generation = files[i].c.inode_generation,
            .content_epoch = files[i].c.content_epoch,
            .record_generation = id ^ ((uint64_t)(i + 1) << 56) ^ files[i].c.ino,
            .len = (uint32_t)files[i].c.size, .data = files[i].data};
    size_t geometry;
    if (efs_pack_build(id, gen, group, rec, n, image, EFS_SEGMENT_MAX_SIZE, &geometry, refs) != EFS_OK) goto out;
    uint32_t flen = (uint32_t)geometry / 2;
    uint8_t *f[EFS_NUM_FRAGMENTS] = {frag, frag + flen, frag + 2 * flen};
    if (efs_encode_segment(image, geometry, geometry, f) != EFS_OK) goto out;
    struct efs_opid owner;
    pk_owner(group, term, &owner);
    owner.seq = id;
    uint32_t members = 0;
    for (uint32_t i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        o->nodes[i] = nodes[i].id;
        members |= 1u << (nodes[i].id - 1);
        efs_hash(f[i], flen, o->checksums[i]);
    }
    if (efs_pack_ledger_init(image, geometry, refs, n, &owner, members, 0, &o->ledger) != EFS_OK) goto out;
    size_t olen;
    cmd[0] = EFS_MD_CMD_PACK_ADMIT; cmd[1] = group;
    if (efs_pack_object_encode(o, cmd + 2, EFS_PACK_OBJECT_MAX_BYTES, &olen) != EFS_OK ||
        server_raft_host_pack_propose(group, cmd, (uint32_t)(2 + olen)) != EFS_OK) {
        STAT_ADD(admit_errors, 1);
        goto out;
    }
    for (uint32_t i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        memset(body, 0, EFS_SEGMENT_PUT_HEADER);
        body[0] = group; body[1] = (uint8_t)i;
        body[4] = (uint8_t)(geometry >> 24); body[5] = (uint8_t)(geometry >> 16);
        body[6] = (uint8_t)(geometry >> 8); body[7] = (uint8_t)geometry;
        wr64(body + 8, id); wr64(body + 16, gen);
        memcpy(body + 24, o->checksums[i], EFS_HASH_SIZE);
        memcpy(body + EFS_SEGMENT_PUT_HEADER, f[i], flen);
        if (pk_put_fragment(nodes, count, o->nodes[i], body, EFS_SEGMENT_PUT_HEADER + flen) != EFS_OK) {
            STAT_ADD(put_errors, 1);
            pk_cancel(group, id, gen);
            goto out;
        }
    }
    STAT_ADD(segments, 1);
    for (uint32_t off = 0; off < n; off += EFS_PACK_SWITCH_MAX) {
        uint32_t m = n - off < EFS_PACK_SWITCH_MAX ? n - off : EFS_PACK_SWITCH_MAX;
        sw->group = group; sw->segment_id = id; sw->segment_generation = gen; sw->count = m;
        for (uint32_t i = 0; i < m; i++) {
            sw->members[i].ino = files[off + i].c.ino;
            sw->members[i].inode_generation = files[off + i].c.inode_generation;
            memcpy(sw->members[i].source_digest, files[off + i].c.source_digest, EFS_HASH_SIZE);
            sw->members[i].ref = refs[off + i];
        }
        size_t wl;
        swire[0] = EFS_MD_CMD_PACK_SWITCH;
        if (efs_pack_switch_encode(sw, swire + 1, 19u + EFS_PACK_SWITCH_MAX * EFS_MD_PACK_SWITCH_MEMBER_BYTES, &wl) != EFS_OK)
            break;
        (void)server_raft_host_pack_propose(group, swire, (uint32_t)(1 + wl));
    }
    /* Whatever did not switch stays a candidate; the rest are done. The
     * ledger, not the proposal verdict, says which members became live. */
    struct efs_pack_object *cur = calloc(1, sizeof(*cur));
    struct efs_kv *kv = server_raft_host_kv();
    if (cur && kv && efs_pack_object_get(kv, group, id, gen, cur) == EFS_OK) {
        for (uint32_t i = 0; i < n; i++) {
            unsigned at;
            if (efs_pack_ledger_slot(&cur->ledger, &refs[i], &at) == EFS_OK &&
                cur->ledger.state[at] == EFS_PACK_LIVE) {
                done[(*ndone)++] = (struct pk_entry){files[i].epoch, files[i].c.ino, 0};
                live_total++;
            }
        }
    }
    free(cur);
    /* Members that could not switch are pending until cancelled. */
    if (live_total < n) pk_cancel(group, id, gen);
    STAT_ADD(files_packed, live_total);
    STAT_ADD(files_cancelled, n - live_total);
out:
    free(rec); free(refs); free(image); free(o); free(frag); free(body); free(cmd); free(sw); free(swire);
    return live_total;
}

static void pk_group(uint8_t group, uint64_t term)
{
    struct efs_kv *kv = server_raft_host_kv();
    if (!kv) return;
    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t count;
    pthread_mutex_lock(&pk.s->lock);
    count = pk.s->node_count;
    memcpy(nodes, pk.s->nodes, count * sizeof(nodes[0]));
    pthread_mutex_unlock(&pk.s->lock);
    if (count < EFS_NUM_FRAGMENTS) return;
    pk_abandon(kv, group, term);

    struct pk_scan sc = {calloc(PK_SCAN_MAX, sizeof(struct pk_entry)), 0};
    struct pk_file *files = calloc(PK_SCAN_MAX, sizeof(*files));
    struct pk_entry *drop = calloc(PK_SCAN_MAX, sizeof(*drop));
    uint32_t nfiles = 0, ndrop = 0;
    if (!sc.e || !files || !drop) goto out;
    uint32_t anchor = efs_pack_group_anchor(group);
    uint8_t prefix[3] = {(uint8_t)(anchor >> 8), (uint8_t)anchor, EFS_KV_KIND_PACK_CANDIDATE};
    if (efs_kv_scan_prefix(kv, prefix, 3, pk_scan_cb, &sc) < 0) goto out;
    STAT_ADD(candidates, sc.n);
    uint64_t now = pk_now_ns();
    for (uint32_t i = 0; i < sc.n; i++) {
        struct pk_file *f = &files[nfiles];
        int rc = efs_meta_pack_candidate(kv, sc.e[i].ino, sc.e[i].gen, &f->c);
        int never = rc == EFS_ERR_STALE || (rc == EFS_OK &&
            (!S_ISREG(f->c.mode) || !f->c.nlink || !f->c.size || f->c.size > EFS_PACK_THRESHOLD ||
             f->c.base.coding_profile_id != EFS_META_PROFILE_K2F1 ||
             (f->c.active_lanes & ~1ull) ||
             efs_raft_shard_group(efs_kv_lane_shard(f->c.ino, 0)) != group));
        if (never) {
            if (getenv("EFS_PACK_DBG"))
                fprintf(stderr, "packer: drop ino=%llu rc=%d mode=%o nlink=%u size=%llu profile=%u ndelta=%u lanes=%llx\n",
                        (unsigned long long)sc.e[i].ino, rc, f->c.mode, f->c.nlink,
                        (unsigned long long)f->c.size, f->c.base.coding_profile_id, f->c.ndelta,
                        (unsigned long long)f->c.active_lanes);
            drop[ndrop++] = sc.e[i];
            continue;
        }
        if (rc != EFS_OK) continue;
        if (f->c.mtime + pk.min_age_ns > now) continue; /* not quiet yet */
        if (efs_lease_any(kv, f->c.ino, f->c.inode_generation) != 0) continue; /* open */
        f->epoch = sc.e[i].epoch;
        nfiles++;
    }
    qsort(files, nfiles, sizeof(*files), pk_file_cmp);
    uint32_t flen = EFS_FRAGMENT_SIZE;
    uint32_t segs = 0, i = 0;
    while (i < nfiles && segs < pk.segments_per_pass && !__atomic_load_n(&pk.stop, __ATOMIC_ACQUIRE)) {
        uint64_t t;
        if (!server_raft_host_pack_leader(group, &t) || t != term) break;
        /* Fill one segment: record and byte caps. */
        uint32_t j = i, used = EFS_PACK_HEADER_BYTES + EFS_HASH_SIZE;
        while (j < nfiles && j - i < EFS_PACK_MAX_RECORDS &&
               used + EFS_PACK_RECORD_BYTES + files[j].c.size <= EFS_SEGMENT_MAX_SIZE) {
            used += EFS_PACK_RECORD_BYTES + (uint32_t)files[j].c.size;
            j++;
        }
        /* Read; a file that cannot be read stays a candidate. */
        uint32_t k = i;
        for (uint32_t x = i; x < j; x++) {
            if (pk_read_file(nodes, count, &files[x], flen) != EFS_OK) {
                STAT_ADD(read_errors, 1);
                free(files[x].data);
                files[x].data = NULL;
                continue;
            }
            if (k != x) { struct pk_file tmp = files[k]; files[k] = files[x]; files[x] = tmp; }
            k++;
        }
        if (k > i) {
            (void)pk_segment(group, term, nodes, count, files + i, k - i, drop, &ndrop);
            segs++;
        }
        i = j;
    }
    pk_unindex(group, drop, ndrop);
    if (segs || ndrop)
        fprintf(stderr, "packer: group=%u term=%llu candidates=%u eligible=%u segments=%u unindexed=%u packed_total=%llu\n",
                group, (unsigned long long)term, sc.n, nfiles, segs, ndrop,
                (unsigned long long)__atomic_load_n(&pk.st.files_packed, __ATOMIC_RELAXED));
out:
    if (files) for (uint32_t x = 0; x < PK_SCAN_MAX; x++) free(files[x].data);
    free(files);
    free(sc.e);
    free(drop);
}

static void *pk_main(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&pk.stop, __ATOMIC_ACQUIRE)) {
        static const uint8_t groups[2] = {EFS_RAFT_GROUP_SHARD, EFS_RAFT_GROUP_SHARD2};
        for (int g = 0; g < 2; g++) {
            uint64_t term = 0;
            if (server_raft_host_pack_leader(groups[g], &term)) pk_group(groups[g], term);
        }
        STAT_ADD(passes, 1);
        for (uint64_t w = 0; w < pk.interval_ms && !__atomic_load_n(&pk.stop, __ATOMIC_ACQUIRE); w += 50)
            usleep(50000);
    }
    return NULL;
}

int server_packer_start(struct efsd_server *s)
{
    if (!s) return -1;
    pk.s = s;
    pk.min_age_ns = env_u64("EFS_PACK_MIN_AGE_MS", 60000, 0) * 1000000ull;
    pk.interval_ms = env_u64("EFS_PACK_INTERVAL_MS", 1000, 1);
    pk.segments_per_pass = (uint32_t)env_u64("EFS_PACK_SEGMENTS_PER_PASS", 8, 1);
    if (pthread_create(&pk.tid, NULL, pk_main, NULL)) return -1;
    pk.started = 1;
    fprintf(stderr, "packer: started min_age_ms=%llu interval_ms=%llu segments_per_pass=%u\n",
            (unsigned long long)(pk.min_age_ns / 1000000ull), (unsigned long long)pk.interval_ms,
            pk.segments_per_pass);
    return 0;
}

void server_packer_stop(void)
{
    if (!pk.started) return;
    __atomic_store_n(&pk.stop, 1, __ATOMIC_RELEASE);
    pthread_join(pk.tid, NULL);
    pk.started = 0;
}
