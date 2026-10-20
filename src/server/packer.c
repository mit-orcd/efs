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
 * Budget: each group has a token bucket of files. Tokens accrue at the idle
 * rate, or at the busy rate while the cluster's foreground commit rate
 * (commits of every hosted group not proposed by the packer) exceeds the
 * busy threshold: the groups share the same nodes, so foreground work in
 * either group makes packing in both yield. A segment is built only when
 * the bucket holds its file count, so segments stay full.
 *
 * While busy the packer yields at the busy floor and catches up when the
 * cluster is idle: packing at a saturating writer's pace costs that writer
 * about a third of its throughput (2026-10-20 A/B). It ramps up only under
 * space pressure, the larger of two signals, each 0 below its start and 1
 * at its full point: unpacked bytes per node (each unpacked small file
 * holds one EFS_FRAGMENT_SIZE fragment on each node; the group's candidate
 * index is counted every PK_COUNT_NS) from EFS_PACK_UNPACKED_GIB to twice
 * that, and quota usage of the group's voters from EFS_PACK_SPACE_START_PCT
 * to EFS_PACK_SPACE_FULL_PCT. The busy rate is floor + (idle - floor) *
 * pressure.
 *
 * Tunables (environment, read at start): EFS_PACK_MIN_AGE_MS (default
 * 60000), EFS_PACK_INTERVAL_MS (1000), EFS_PACK_SEGMENTS_PER_PASS (8),
 * EFS_PACK_FILES_PER_S (5000), EFS_PACK_BUSY_FILES_PER_S (50),
 * EFS_PACK_BUSY_COMMITS_PER_S (100), EFS_PACK_UNPACKED_GIB (64),
 * EFS_PACK_SPACE_START_PCT (50), EFS_PACK_SPACE_FULL_PCT (80),
 * EFS_PACK_BACKFILL_SHARDS (64 inode
 * shards scanned per pass until the group's backfill is done). None of
 * them turns the packer off:
 * rates and counts below 1 are raised to 1 (the minimum age may be 0 for
 * tests). */
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
#define PK_FAIL_BACKOFF_NS (30ull * 1000000000ull) /* after a failed admission or upload */

static struct {
    struct efsd_server *s;
    pthread_t tid;
    int started, stop;
    uint64_t min_age_ns, interval_ms;
    uint32_t segments_per_pass;
    double idle_rate, busy_rate, busy_commits;
    uint64_t unpacked_start; /* bytes per node */
    uint64_t compact_idle_ns, compact_cold_ns;
    uint32_t compact_idle_segs, compact_daily_start, compact_daily_min; /* minutes, UTC */
    double space_start, space_full; /* quota fractions */
    uint32_t backfill_shards;
    struct {
        uint64_t last_commit, last_ns, own;
        uint64_t backoff_until_ns; /* after a failed copy: leave the members alone */
        double tokens, fg_rate, rate;
        uint64_t lag_ns; /* oldest eligible candidate's wait beyond the minimum age */
        uint64_t backlog, count_ns; /* candidate-index entries, when last counted */
        uint64_t fsum_ns;           /* last fragmentation summary */
        uint64_t idle_since_ns, compact_batch_ns; /* compaction schedule */
        double pressure, used_frac;
        int busy;
    } g[2];
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

/* Crash injection (EFS_FAULTS builds only; production has no such hook):
 * EFS_PACK_CRASH=<point> and EFS_PACK_CRASH_NODE=<id> make that daemon
 * exit as if killed the first time it reaches the point. Points:
 * admitted (before any PUT), uploaded (fragments durable, before the
 * switch), switched (before unindex), retire_accepted (segments in the
 * journals, before PACK_RETIRE), gc_accepted (superseded fragments in the
 * journals, before GC_ACK). */
void server_pack_crash_point(const char *point)
{
#ifdef EFS_FAULTS
    static int fired;
    const char *want = getenv("EFS_PACK_CRASH"), *node = getenv("EFS_PACK_CRASH_NODE");
    if (fired || !want || strcmp(want, point) || !pk.s) return;
    if (node && (uint32_t)strtoul(node, NULL, 10) != pk.s->id) return;
    fired = 1;
    fprintf(stderr, "packer: injected crash at %s\n", point);
    fflush(stderr);
    _exit(137);
#else
    (void)point;
#endif
}

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
static int pk_gi(uint8_t group) { return group == EFS_RAFT_GROUP_SHARD ? 0 : 1; }
/* Packer proposals are excluded from the foreground commit rate. */
static int pk_propose(uint8_t group, const uint8_t *cmd, uint32_t len)
{
    __atomic_fetch_add(&pk.g[pk_gi(group)].own, 1, __ATOMIC_RELAXED);
    return server_raft_host_pack_propose(group, cmd, len);
}
static void pk_cancel(uint8_t group, uint64_t id, uint64_t gen)
{
    uint8_t cmd[18];
    cmd[0] = EFS_MD_CMD_PACK_CANCEL; cmd[1] = group;
    wr64(cmd + 2, id); wr64(cmd + 10, gen);
    (void)pk_propose(group, cmd, sizeof(cmd));
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
    if (rd64(k + 3) == EFS_PACK_CURSOR_EPOCH) return 0; /* backfill cursor */
    sc->e[sc->n++] = (struct pk_entry){rd64(k + 3), rd64(k + 11), rd64(v)};
    return sc->n == PK_SCAN_MAX;
}
#define PK_COUNT_NS (30ull * 1000000000ull)
static int pk_count_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    (void)v; (void)vl;
    if (kl == EFS_PACK_CANDIDATE_KEY_BYTES && k[2] == EFS_KV_KIND_PACK_CANDIDATE &&
        rd64(k + 3) != EFS_PACK_CURSOR_EPOCH)
        (*(uint64_t *)u)++;
    return 0;
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
static int pk_read_segment(const struct efs_node *nodes, uint32_t count, const struct efs_pack_object *o,
                           uint8_t *image);
/* A packed base's bytes: its record, read back from the segment. */
static int pk_read_packed_base(const struct efs_node *nodes, uint32_t count, struct pk_file *f)
{
    struct efs_kv *kv = server_raft_host_kv();
    const struct efs_pack_ref *r = &f->c.base.packed;
    struct efs_pack_object *o = malloc(sizeof(*o));
    uint8_t *image = malloc(EFS_SEGMENT_MAX_SIZE);
    int rc = kv && o && image ? EFS_OK : EFS_ERR_NOMEM;
    if (rc == EFS_OK) rc = efs_pack_object_get(kv, r->publication_group, r->segment_id, r->segment_generation, o);
    if (rc == EFS_OK) rc = pk_read_segment(nodes, count, o, image);
    if (rc == EFS_OK) {
        const uint8_t *data;
        uint32_t len;
        rc = efs_pack_read(image, o->ledger.geometry, r, &data, &len);
        if (rc == EFS_OK) memcpy(f->data, data, len < f->c.size ? len : f->c.size);
    }
    free(o);
    free(image);
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
    if (rc == EFS_OK && f->c.base.coding_profile_id == EFS_META_PROFILE_PACK) {
        rc = pk_read_packed_base(nodes, count, f);
    } else if (rc == EFS_OK && f->c.base.generation) {
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
/* Reading a segment's files one after another cost about a third of a
 * millisecond each (a fragment GET per file) and capped the packer near
 * 1,000 files/s: read them on PK_READ_THREADS threads. rcs[x] gets each
 * file's result. */
#define PK_READ_THREADS 16u
struct pk_reader {
    const struct efs_node *nodes;
    uint32_t count, flen, n, next;
    struct pk_file *files;
    int *rcs;
};
static void *pk_reader_run(void *arg)
{
    struct pk_reader *r = arg;
    for (;;) {
        uint32_t x = __atomic_fetch_add(&r->next, 1, __ATOMIC_RELAXED);
        if (x >= r->n) return NULL;
        r->rcs[x] = pk_read_file(r->nodes, r->count, &r->files[x], r->flen);
    }
}
static void pk_read_files(const struct efs_node *nodes, uint32_t count, struct pk_file *files,
                          uint32_t n, uint32_t flen, int *rcs)
{
    struct pk_reader r = {nodes, count, flen, n, 0, files, rcs};
    pthread_t t[PK_READ_THREADS];
    uint32_t started = 0, want = n < PK_READ_THREADS ? n : PK_READ_THREADS;
    for (uint32_t i = 1; i < want; i++)
        if (pthread_create(&t[started], NULL, pk_reader_run, &r) == 0) started++;
    pk_reader_run(&r); /* this thread reads too; a failed create only slows it */
    for (uint32_t i = 0; i < started; i++) pthread_join(t[i], NULL);
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
struct pk_put {
    const struct efs_node *nodes;
    uint32_t count, id;
    const uint8_t *body;
    uint32_t len;
    int rc;
};
static void *pk_put_run(void *arg)
{
    struct pk_put *p = arg;
    p->rc = pk_put_fragment(p->nodes, p->count, p->id, p->body, p->len);
    return NULL;
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
        if (pk_propose(group, cmd, 4u + 16u * m) == EFS_OK) STAT_ADD(unindexed, m);
        free(cmd);
        e += m;
        n -= m;
    }
}

/* Pack files[0..n) (already read) into one segment. Returns the number of
 * files switched; *done receives the index entries to drop. */
/* Segment ids: time-ordered and node-tagged as before, now also unique
 * across the concurrent segment jobs of one pass. */
static uint64_t pk_segment_id(void)
{
    static uint64_t last;
    uint64_t want = pk_now_ns() << 8, prev = __atomic_load_n(&last, __ATOMIC_RELAXED);
    for (;;) {
        uint64_t next = want > prev ? want : prev + 256;
        if (__atomic_compare_exchange_n(&last, &prev, next, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return next | (pk.s->id & 0xff);
    }
}
static uint32_t pk_segment(uint8_t group, uint64_t term, const struct efs_node *nodes, uint32_t count,
                           struct pk_file *files, uint32_t n, struct pk_entry *done, uint32_t *ndone,
                           uint32_t flags)
{
    uint32_t live_total = 0;
    struct efs_pack_record *rec = calloc(n, sizeof(*rec));
    struct efs_pack_ref *refs = calloc(n, sizeof(*refs));
    uint8_t *image = malloc(EFS_SEGMENT_MAX_SIZE);
    struct efs_pack_object *o = calloc(1, sizeof(*o));
    uint8_t *frag = malloc(EFS_SEGMENT_MAX_SIZE / 2 * 3);
    uint8_t *body = malloc((size_t)EFS_NUM_FRAGMENTS * (EFS_SEGMENT_PUT_HEADER + EFS_SEGMENT_MAX_SIZE / 2));
    uint8_t *cmd = malloc(2u + EFS_PACK_OBJECT_MAX_BYTES);
    struct efs_pack_switch *sw = calloc(1, sizeof(*sw));
    uint8_t *swire = malloc(1u + 19u + EFS_PACK_SWITCH_MAX * EFS_MD_PACK_SWITCH_MEMBER_BYTES);
    if (!rec || !refs || !image || !o || !frag || !body || !cmd || !sw || !swire) goto out;
    uint64_t id = pk_segment_id(), gen = 1;
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
    if (efs_pack_ledger_init(image, geometry, refs, n, &owner, members, flags, &o->ledger) != EFS_OK) goto out;
    size_t olen;
    cmd[0] = EFS_MD_CMD_PACK_ADMIT; cmd[1] = group;
    if (efs_pack_object_encode(o, cmd + 2, EFS_PACK_OBJECT_MAX_BYTES, &olen) != EFS_OK ||
        pk_propose(group, cmd, (uint32_t)(2 + olen)) != EFS_OK) {
        STAT_ADD(admit_errors, 1);
        pk.g[pk_gi(group)].backoff_until_ns = pk_now_ns() + PK_FAIL_BACKOFF_NS;
        goto out;
    }
    server_pack_crash_point("admitted");
    /* The three fragments go out in parallel; all must be durable. */
    struct pk_put puts[EFS_NUM_FRAGMENTS];
    pthread_t pt[EFS_NUM_FRAGMENTS];
    int pstarted[EFS_NUM_FRAGMENTS] = {0}, put_ok = 1;
    for (uint32_t i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        uint8_t *b = body + (size_t)i * (EFS_SEGMENT_PUT_HEADER + EFS_SEGMENT_MAX_SIZE / 2);
        memset(b, 0, EFS_SEGMENT_PUT_HEADER);
        b[0] = group; b[1] = (uint8_t)i;
        b[4] = (uint8_t)(geometry >> 24); b[5] = (uint8_t)(geometry >> 16);
        b[6] = (uint8_t)(geometry >> 8); b[7] = (uint8_t)geometry;
        wr64(b + 8, id); wr64(b + 16, gen);
        memcpy(b + 24, o->checksums[i], EFS_HASH_SIZE);
        memcpy(b + EFS_SEGMENT_PUT_HEADER, f[i], flen);
        puts[i] = (struct pk_put){nodes, count, o->nodes[i], b, EFS_SEGMENT_PUT_HEADER + flen, EFS_ERR_IO};
        pstarted[i] = i > 0 && pthread_create(&pt[i], NULL, pk_put_run, &puts[i]) == 0;
    }
    for (uint32_t i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!pstarted[i]) pk_put_run(&puts[i]);
        else pthread_join(pt[i], NULL);
        if (puts[i].rc != EFS_OK) put_ok = 0;
    }
    if (!put_ok) {
        STAT_ADD(put_errors, 1);
        /* A member is unreachable (possibly down before the heartbeat
         * says so). Retrying every pass would keep failing and push
         * this node's peer-connection cooldown for that member past its
         * return, starving foreground traffic to it: back off. */
        pk.g[pk_gi(group)].backoff_until_ns = pk_now_ns() + PK_FAIL_BACKOFF_NS;
        pk_cancel(group, id, gen);
        goto out;
    }
    STAT_ADD(segments, 1);
    server_pack_crash_point("uploaded");
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
        (void)pk_propose(group, swire, (uint32_t)(1 + wl));
    }
    server_pack_crash_point("switched");
    /* Whatever did not switch stays a candidate; the rest are done. The
     * ledger, not the proposal verdict, says which members became live. */
    struct efs_pack_object *cur = calloc(1, sizeof(*cur));
    struct efs_kv *kv = server_raft_host_kv();
    if (cur && kv && efs_pack_object_get(kv, group, id, gen, cur) == EFS_OK) {
        for (uint32_t i = 0; i < n; i++) {
            unsigned at;
            if (efs_pack_ledger_slot(&cur->ledger, &refs[i], &at) == EFS_OK &&
                cur->ledger.state[at] == EFS_PACK_LIVE) {
                if (done) done[(*ndone)++] = (struct pk_entry){files[i].epoch, files[i].c.ino, 0};
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

/* ---- compaction: copy survivors of mostly-dead segments forward ---- */
#define PK_COMPACT_MAX 64u
#define PK_SMALL_BYTES (8u * 1024u)   /* underfilled: merged with other small ones */
struct pk_seg {
    struct efs_pack_object o;
    double score;
    int holes;
};
/* Compaction priority, higher first: favour tiny holes (a high live
 * fraction) and cold segments (compacted output, or older than the cold
 * age; segment ids carry their creation time, modulo 2^56 ns). A cluster
 * left idle long enough compacts every holed segment in this order. */
static double pk_compact_priority(uint64_t live_bytes, uint64_t total_bytes, uint64_t age_ns, int cold_flag)
{
    double live = total_bytes ? (double)live_bytes / (double)total_bytes : 0;
    double cold = cold_flag || !pk.compact_cold_ns ? 1.0 : (double)age_ns / (double)pk.compact_cold_ns;
    if (cold > 1.0) cold = 1.0;
    return 0.5 * live + 0.5 * cold;
}
static uint64_t pk_segment_age_ns(uint64_t segment_id, uint64_t now_ns)
{
    const uint64_t m = (1ull << 56) - 1;
    return ((now_ns & m) - (segment_id >> 8)) & m;
}
/* The whole index is scanned; the PK_COMPACT_MAX best candidates are kept:
 * every segment with a hole, and underfilled ones (merged together). */
struct pk_cscan { struct pk_seg *s; uint32_t n, small; uint64_t now; struct efs_pack_object tmp; };
static int pk_cscan_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    struct pk_cscan *c = u;
    if (kl != EFS_PACK_OBJECT_KEY_BYTES || k[2] != EFS_KV_KIND_PACK_SEGMENT) return 0;
    if (efs_pack_object_decode(v, vl, &c->tmp) != EFS_OK || c->tmp.ledger.phase != EFS_PACK_ACTIVE) return 0;
    struct efs_pack_ledger_usage u2;
    efs_pack_ledger_usage(&c->tmp.ledger, &u2);
    if (u2.pending || !u2.live) return 0;
    int holes = u2.dead_bytes > 0;
    int small = u2.live_bytes < PK_SMALL_BYTES;
    if (!holes && !small) return 0;
    double score = pk_compact_priority(u2.live_bytes, u2.total_bytes,
                                       pk_segment_age_ns(c->tmp.ledger.segment_id, c->now),
                                       (c->tmp.ledger.flags & EFS_PACK_LEDGER_F_COLD) != 0);
    uint32_t at = c->n;
    if (c->n == PK_COMPACT_MAX) { /* replace the weakest kept candidate */
        at = 0;
        for (uint32_t i = 1; i < c->n; i++) if (c->s[i].score < c->s[at].score) at = i;
        if (c->s[at].score >= score) return 0;
        c->small -= !c->s[at].holes;
    } else {
        c->n++;
    }
    c->s[at].o = c->tmp;
    c->s[at].score = score;
    c->s[at].holes = holes;
    c->small += !holes;
    return 0;
}
/* Fragmentation summary over a group's active segments, logged with the
 * backlog count: how much of the packed bytes are dead, and how many
 * segments compaction would take (holes) or merge (small). */
struct pk_fsum { uint64_t segs, live, dead, total, holes, small; struct efs_pack_object tmp; };
static int pk_fsum_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    struct pk_fsum *f = u;
    if (kl != EFS_PACK_OBJECT_KEY_BYTES || k[2] != EFS_KV_KIND_PACK_SEGMENT) return 0;
    if (efs_pack_object_decode(v, vl, &f->tmp) != EFS_OK || f->tmp.ledger.phase != EFS_PACK_ACTIVE) return 0;
    struct efs_pack_ledger_usage u2;
    efs_pack_ledger_usage(&f->tmp.ledger, &u2);
    f->segs++;
    f->live += u2.live_bytes;
    f->dead += u2.dead_bytes;
    f->total += u2.total_bytes;
    if (u2.live && u2.dead_bytes) f->holes++;
    else if (u2.live && u2.live_bytes < PK_SMALL_BYTES) f->small++;
    return 0;
}
static int pk_seg_cmp(const void *a, const void *b)
{
    const struct pk_seg *x = a, *y = b;
    return x->score < y->score ? 1 : x->score > y->score ? -1 : 0;
}
static int pk_get_segfrag(const struct efs_node *nodes, uint32_t count, const struct efs_pack_object *o,
                          uint32_t fi, uint8_t *out, uint32_t len)
{
    uint32_t id = o->nodes[fi], group = o->ledger.publication_group;
    uint8_t sum[EFS_HASH_SIZE];
    int rc;
    if (id == pk.s->id) {
        rc = server_segment_read(pk.s, group, o->ledger.segment_id, o->ledger.segment_generation,
                                 fi, len, out, sum);
    } else {
        const struct efs_node *n = pk_node(nodes, count, id);
        if (!n) return EFS_ERR_IO;
        struct efs_conn *pc = server_peer_conn_get(n->addr, n->port);
        if (!pc) return EFS_ERR_NET;
        uint8_t req[EFS_SEGMENT_GET_HEADER] = {0};
        req[0] = (uint8_t)group; req[1] = (uint8_t)fi;
        uint32_t g = o->ledger.geometry;
        req[4] = (uint8_t)(g >> 24); req[5] = (uint8_t)(g >> 16); req[6] = (uint8_t)(g >> 8); req[7] = (uint8_t)g;
        wr64(req + 8, o->ledger.segment_id); wr64(req + 16, o->ledger.segment_generation);
        uint8_t type;
        void *reply = NULL;
        uint32_t rlen = 0;
        if (efs_conn_send_msg(pc, EFS_MSG_GET_SEGMENT, req, sizeof(req)) ||
            efs_conn_recv_msg(pc, &type, &reply, &rlen)) {
            server_peer_conn_drop(n->addr, n->port, pc);
            free(reply);
            return EFS_ERR_NET;
        }
        server_peer_conn_release(n->addr, n->port, pc);
        const uint8_t *r = reply;
        rc = type == EFS_MSG_GET_SEGMENT_REPLY && rlen == EFS_SEGMENT_GET_REPLY + len &&
             !r[0] && !r[1] && !r[2] && !r[3] ? EFS_OK : EFS_ERR_IO;
        if (rc == EFS_OK) { memcpy(sum, r + 8, EFS_HASH_SIZE); memcpy(out, r + EFS_SEGMENT_GET_REPLY, len); }
        free(reply);
    }
    if (rc != EFS_OK) return rc;
    uint8_t h[EFS_HASH_SIZE];
    efs_hash(out, len, h);
    return memcmp(h, o->checksums[fi], EFS_HASH_SIZE) || memcmp(sum, o->checksums[fi], EFS_HASH_SIZE)
           ? EFS_ERR_CHECKSUM : EFS_OK;
}
/* Any two of the three fragments; the image is validated. */
static int pk_read_segment(const struct efs_node *nodes, uint32_t count, const struct efs_pack_object *o,
                           uint8_t *image)
{
    uint32_t g = o->ledger.geometry, flen = g / 2;
    uint8_t *buf = malloc((size_t)flen * EFS_NUM_FRAGMENTS);
    if (!buf) return EFS_ERR_NOMEM;
    uint8_t *f[EFS_NUM_FRAGMENTS] = {buf, buf + flen, buf + 2 * flen};
    int have[EFS_NUM_FRAGMENTS] = {0}, n = 0;
    for (uint32_t fi = 0; fi < EFS_NUM_FRAGMENTS && n < 2; fi++)
        if (pk_get_segfrag(nodes, count, o, fi, f[fi], flen) == EFS_OK) { have[fi] = 1; n++; }
    int a = -1, b = -1, missing = -1;
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!have[i]) { if (missing < 0) missing = i; }
        else if (a < 0) a = i; else if (b < 0) b = i;
    }
    int rc = n == 2 && missing >= 0 ? efs_decode_segment(f, g, a, b, missing, image, g) : EFS_ERR_DECODE;
    if (rc == EFS_OK) rc = efs_pack_validate(image, g);
    free(buf);
    return rc;
}
static uint32_t pk_compact(struct efs_kv *kv, uint8_t group, uint64_t term,
                           const struct efs_node *nodes, uint32_t count)
{
    uint32_t moved = 0;
    struct pk_cscan sc;
    memset(&sc, 0, sizeof(sc));
    sc.now = pk_now_ns();
    sc.s = calloc(PK_COMPACT_MAX, sizeof(struct pk_seg));
    struct pk_file *files = calloc(EFS_PACK_MAX_RECORDS, sizeof(*files));
    uint8_t **images = calloc(PK_COMPACT_MAX, sizeof(*images));
    uint32_t nfiles = 0, used = EFS_PACK_HEADER_BYTES + EFS_HASH_SIZE;
    if (!sc.s || !files || !images) goto out;
    uint32_t anchor = efs_pack_group_anchor(group);
    uint8_t prefix[3] = {(uint8_t)(anchor >> 8), (uint8_t)anchor, EFS_KV_KIND_PACK_SEGMENT};
    if (efs_kv_scan_prefix(kv, prefix, 3, pk_cscan_cb, &sc) < 0 || !sc.n) goto out;
    /* A lone underfilled segment has nothing to merge with. */
    if (sc.n == 1 && sc.small == 1) goto out;
    qsort(sc.s, sc.n, sizeof(sc.s[0]), pk_seg_cmp);
    struct efs_pack_candidate *c = calloc(1, sizeof(*c));
    if (!c) goto out;
    uint32_t used_segs = 0;
    for (uint32_t si = 0; si < sc.n && nfiles < EFS_PACK_MAX_RECORDS; si++) {
        const struct efs_pack_object *o = &sc.s[si].o;
        images[si] = malloc(EFS_SEGMENT_MAX_SIZE);
        if (!images[si] || pk_read_segment(nodes, count, o, images[si]) != EFS_OK) {
            STAT_ADD(read_errors, 1);
            continue;
        }
        uint32_t nrec;
        if (efs_pack_record_count(images[si], o->ledger.geometry, &nrec) != EFS_OK) continue;
        uint32_t before = nfiles;
        for (uint32_t r = 0; r < nrec && nfiles < EFS_PACK_MAX_RECORDS; r++) {
            struct efs_pack_ref ref;
            unsigned at;
            if (efs_pack_record_at(images[si], o->ledger.geometry, r, &ref) != EFS_OK ||
                efs_pack_ledger_slot(&o->ledger, &ref, &at) != EFS_OK ||
                o->ledger.state[at] != EFS_PACK_LIVE) continue;
            if (used + EFS_PACK_RECORD_BYTES + ref.len > EFS_SEGMENT_MAX_SIZE) break;
            /* The file must still point at exactly this record. */
            uint8_t x[EFS_PACK_REF_BYTES], y[EFS_PACK_REF_BYTES];
            if (efs_meta_pack_candidate(kv, ref.ino, ref.inode_generation, c) != EFS_OK ||
                c->base.coding_profile_id != EFS_META_PROFILE_PACK || c->ndelta || !c->nlink ||
                efs_pack_ref_encode(&c->base.packed, x) != EFS_OK ||
                efs_pack_ref_encode(&ref, y) != EFS_OK || memcmp(x, y, sizeof(x)) ||
                efs_lease_any(kv, ref.ino, ref.inode_generation) != 0) continue;
            struct pk_file *f = &files[nfiles];
            f->c = *c;
            f->c.size = ref.len;
            f->data = malloc(ref.len);
            if (!f->data) break;
            memcpy(f->data, images[si] + ref.offset, ref.len);
            used += EFS_PACK_RECORD_BYTES + ref.len;
            nfiles++;
        }
        if (nfiles > before) used_segs++;
    }
    free(c);
    /* Worth a copy: it frees a segment with holes, or merges at least two
     * underfilled ones. Scheduled maintenance: no packing tokens. */
    int worth = used_segs >= 2 || (used_segs == 1 && sc.s[0].holes);
    if (nfiles && worth) {
        qsort(files, nfiles, sizeof(*files), pk_file_cmp);
        moved = pk_segment(group, term, nodes, count, files, nfiles, NULL, NULL, EFS_PACK_LEDGER_F_COLD);
        STAT_ADD(compacted, moved);
        fprintf(stderr, "packer: compact group=%u sources=%u moved=%u/%u\n", group, used_segs, moved, nfiles);
    }
out:
    if (files) for (uint32_t i = 0; i < EFS_PACK_MAX_RECORDS; i++) free(files[i].data);
    if (images) for (uint32_t i = 0; i < PK_COMPACT_MAX; i++) free(images[i]);
    free(images);
    free(files);
    free(sc.s);
    return moved;
}

/* ---- backfill: index small files written before the packer ---- */
struct pk_inos { efs_ino_t *v; uint32_t n, cap; };
static int pk_inos_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v, uint32_t vl)
{
    struct pk_inos *a = u;
    (void)v; (void)vl;
    if (kl != 11 || k[2] != EFS_KV_KIND_INODE) return 0;
    if (a->n == a->cap) {
        uint32_t cap = a->cap ? a->cap * 2 : 1024;
        efs_ino_t *nv = realloc(a->v, cap * sizeof(*nv));
        if (!nv) return 1;
        a->v = nv; a->cap = cap;
    }
    a->v[a->n++] = rd64(k + 3);
    return 0;
}
static int pk_index_flush(uint8_t group, struct efs_pack_index_item *items, uint32_t n,
                          uint32_t next_shard, int done)
{
    uint8_t *cmd = malloc(9u + 24u * n);
    if (!cmd) return EFS_ERR_NOMEM;
    cmd[0] = EFS_MD_CMD_PACK_INDEX; cmd[1] = group;
    cmd[2] = (uint8_t)(n >> 8); cmd[3] = (uint8_t)n;
    cmd[4] = (uint8_t)(next_shard >> 24); cmd[5] = (uint8_t)(next_shard >> 16);
    cmd[6] = (uint8_t)(next_shard >> 8); cmd[7] = (uint8_t)next_shard; cmd[8] = done ? 1 : 0;
    for (uint32_t i = 0; i < n; i++) {
        wr64(cmd + 9 + 24 * i, items[i].epoch); wr64(cmd + 17 + 24 * i, items[i].ino);
        wr64(cmd + 25 + 24 * i, items[i].inode_generation);
    }
    int rc = pk_propose(group, cmd, 9u + 24u * n);
    free(cmd);
    if (rc == EFS_OK) STAT_ADD(backfilled, n);
    return rc;
}
static void pk_backfill(struct efs_kv *kv, uint8_t group)
{
    uint32_t next = 0;
    int done = 0;
    if (efs_meta_pack_backfill_cursor(kv, group, &next, &done) != EFS_OK || done) return;
    struct efs_pack_index_item *items = calloc(EFS_PACK_INDEX_MAX, sizeof(*items));
    struct efs_pack_candidate *c = calloc(1, sizeof(*c));
    struct pk_inos a = {0};
    uint32_t n = 0, end = next + pk.backfill_shards;
    if (end > EFS_KV_SHARD_MASK + 1u) end = EFS_KV_SHARD_MASK + 1u;
    if (!items || !c) goto out;
    for (uint32_t s = next; s < end; s++) {
        if (efs_raft_shard_group(s) != group) continue;
        uint8_t prefix[3] = {(uint8_t)(s >> 8), (uint8_t)s, EFS_KV_KIND_INODE};
        a.n = 0;
        if (efs_kv_scan_prefix(kv, prefix, 3, pk_inos_cb, &a) < 0) goto out;
        for (uint32_t i = 0; i < a.n; i++) {
            struct efs_meta_row row;
            if (efs_meta_apply_get_inode(kv, a.v[i], &row) != EFS_OK || !S_ISREG(row.mode)) continue;
            if (efs_meta_pack_candidate(kv, a.v[i], row.generation, c) != EFS_OK) continue;
            if (!c->nlink || !c->size || c->size > EFS_PACK_THRESHOLD || (c->active_lanes & ~1ull) ||
                c->base.coding_profile_id != EFS_META_PROFILE_K2F1) continue;
            items[n++] = (struct efs_pack_index_item){c->mtime / 1000000000ull / EFS_PACK_EPOCH_S,
                                                      c->ino, c->inode_generation};
            if (n == EFS_PACK_INDEX_MAX) {
                /* The cursor stays at this shard until its last batch. */
                if (pk_index_flush(group, items, n, s, 0) != EFS_OK) goto out;
                n = 0;
            }
        }
    }
    (void)pk_index_flush(group, items, n, end, end > EFS_KV_SHARD_MASK);
    if (end > EFS_KV_SHARD_MASK)
        fprintf(stderr, "packer: backfill group=%u complete, %llu files indexed\n", group,
                (unsigned long long)__atomic_load_n(&pk.st.backfilled, __ATOMIC_RELAXED));
out:
    free(a.v);
    free(items);
    free(c);
}

#define PK_SEGMENTS_MAX 32u
struct pk_job {
    uint32_t start, end, n, ndone;
    uint8_t group;
    uint64_t term;
    const struct efs_node *nodes;
    uint32_t count;
    struct pk_file *files;
    struct pk_entry *done;
};
static void *pk_job_run(void *arg)
{
    struct pk_job *j = arg;
    j->ndone = 0;
    (void)pk_segment(j->group, j->term, j->nodes, j->count, j->files, j->n, j->done, &j->ndone, 0);
    return NULL;
}

/* Compaction schedule: output segments to build in this pass.
 * - Idle: once the cluster has not been busy for compact_idle_ns, a batch
 *   of compact_idle_segs every compact_idle_ns while it stays idle (reads
 *   are not commits, so a read-only cluster counts as idle).
 * - Daily: inside [compact_daily_start, +compact_daily_min) UTC, a larger
 *   batch every pass, but only while the cluster is not busy.
 * Busy: nothing; the idle clock restarts. */
static uint32_t pk_compact_due(int busy, uint64_t now, uint64_t *idle_since, uint64_t *last_batch,
                               uint32_t minute_of_day)
{
    if (busy) { *idle_since = 0; return 0; }
    if (!*idle_since) *idle_since = now;
    uint32_t end = pk.compact_daily_start + pk.compact_daily_min;
    int daily = pk.compact_daily_min &&
                (minute_of_day >= pk.compact_daily_start ? minute_of_day < end : minute_of_day + 1440 < end);
    if (daily) return pk.segments_per_pass;
    if (now - *idle_since >= pk.compact_idle_ns && now - *last_batch >= pk.compact_idle_ns) {
        *last_batch = now;
        return pk.compact_idle_segs;
    }
    return 0;
}
static uint32_t pk_minute_of_day(void)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    return (uint32_t)(tm.tm_hour * 60 + tm.tm_min);
}

/* One pass over a group. Returns 1 when it stopped at the per-pass segment
 * cap with eligible files and budget left, so the caller does not sleep. */
static int pk_group(uint8_t group, uint64_t term)
{
    if (pk_now_ns() < pk.g[pk_gi(group)].backoff_until_ns) return 0;
    struct efs_kv *kv = server_raft_host_kv();
    if (!kv) return 0;
    struct efs_node nodes[EFS_MAX_NODES];
    uint32_t count;
    pthread_mutex_lock(&pk.s->lock);
    count = pk.s->node_count;
    memcpy(nodes, pk.s->nodes, count * sizeof(nodes[0]));
    pthread_mutex_unlock(&pk.s->lock);
    /* Segments live on voters of their group (the nodes that can check a
     * PUT and a reclaim against the object), and only on live ones: the
     * first EFS_NUM_FRAGMENTS of them are moved to the front. */
    {
        uint32_t voters = server_raft_host_group_voters(group), m = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t id = nodes[i].id;
            int voter = id && id <= EFS_MAX_NODES && (voters & (1u << (id - 1)));
            int up = id == pk.s->id || !nodes[i].down_until_ms;
            if (voter && up) { struct efs_node t = nodes[m]; nodes[m] = nodes[i]; nodes[i] = t; m++; }
        }
        if (m < EFS_NUM_FRAGMENTS) {
            pk_abandon(kv, group, term);
            return 0; /* not enough live voters to place a segment */
        }
    }
    pk_abandon(kv, group, term);
    pk_backfill(kv, group);

    struct pk_scan sc = {calloc(PK_SCAN_MAX, sizeof(struct pk_entry)), 0};
    struct pk_file *files = calloc(PK_SCAN_MAX, sizeof(*files));
    /* drop: unindex list, then per-segment done lists for the jobs. */
    struct pk_entry *drop = calloc(PK_SCAN_MAX + (size_t)PK_SEGMENTS_MAX * EFS_PACK_MAX_RECORDS, sizeof(*drop));
    uint32_t nfiles = 0, ndrop = 0;
    int more = 0;
    uint64_t oldest = 0, t_scan = pk_now_ns();
    if (!sc.e || !files || !drop) goto out;
    uint32_t anchor = efs_pack_group_anchor(group);
    uint8_t prefix[3] = {(uint8_t)(anchor >> 8), (uint8_t)anchor, EFS_KV_KIND_PACK_CANDIDATE};
    if (efs_kv_scan_prefix(kv, prefix, 3, pk_scan_cb, &sc) < 0) goto out;
    /* The whole index, for the unpacked-bytes signal: not every pass. */
    if (sc.n < PK_SCAN_MAX) {
        pk.g[pk_gi(group)].backlog = sc.n; /* the scan saw it all */
        pk.g[pk_gi(group)].count_ns = pk_now_ns();
    } else if (pk_now_ns() >= pk.g[pk_gi(group)].count_ns + PK_COUNT_NS) {
        uint64_t n = 0;
        if (efs_kv_scan_prefix(kv, prefix, 3, pk_count_cb, &n) >= 0) pk.g[pk_gi(group)].backlog = n;
        pk.g[pk_gi(group)].count_ns = pk_now_ns();
    }
    if (pk_now_ns() >= pk.g[pk_gi(group)].fsum_ns + PK_COUNT_NS) {
        struct pk_fsum *fs = calloc(1, sizeof(*fs));
        uint8_t sp[3] = {(uint8_t)(anchor >> 8), (uint8_t)anchor, EFS_KV_KIND_PACK_SEGMENT};
        if (fs && efs_kv_scan_prefix(kv, sp, 3, pk_fsum_cb, fs) >= 0)
            fprintf(stderr, "packer: segments group=%u active=%llu total_kib=%llu live_kib=%llu dead_kib=%llu "
                    "holes=%llu small=%llu backlog=%llu compacted_total=%llu\n", group,
                    (unsigned long long)fs->segs, (unsigned long long)(fs->total >> 10),
                    (unsigned long long)(fs->live >> 10), (unsigned long long)(fs->dead >> 10),
                    (unsigned long long)fs->holes, (unsigned long long)fs->small,
                    (unsigned long long)pk.g[pk_gi(group)].backlog,
                    (unsigned long long)__atomic_load_n(&pk.st.compacted, __ATOMIC_RELAXED));
        free(fs);
        pk.g[pk_gi(group)].fsum_ns = pk_now_ns();
    }
    STAT_ADD(candidates, sc.n);
    uint64_t now = pk_now_ns();
    for (uint32_t i = 0; i < sc.n; i++) {
        struct pk_file *f = &files[nfiles];
        int rc = efs_meta_pack_candidate(kv, sc.e[i].ino, sc.e[i].gen, &f->c);
        int never = rc == EFS_ERR_STALE || (rc == EFS_OK &&
            (!S_ISREG(f->c.mode) || !f->c.nlink || !f->c.size || f->c.size > EFS_PACK_THRESHOLD ||
             (f->c.base.coding_profile_id != EFS_META_PROFILE_K2F1 &&
              !(f->c.base.coding_profile_id == EFS_META_PROFILE_PACK && f->c.ndelta)) ||
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
        if (!oldest || f->c.mtime < oldest) oldest = f->c.mtime;
        nfiles++;
    }
    pk.g[pk_gi(group)].lag_ns = oldest && now > oldest + pk.min_age_ns ? now - oldest - pk.min_age_ns : 0;
    qsort(files, nfiles, sizeof(*files), pk_file_cmp);
    uint32_t flen = EFS_FRAGMENT_SIZE;
    uint32_t segs = 0, i = 0;
    uint64_t t_read = 0, t_seg = 0, t_pass = pk_now_ns();
    /* Choose this pass's segments (record and byte caps, budget), read all
     * their files in one parallel batch, then build, upload and switch the
     * segments concurrently: one after another they cost ~90 ms each. */
    struct pk_job jobs[PK_SEGMENTS_MAX];
    uint32_t njobs = 0;
    uint64_t t;
    if (server_raft_host_pack_leader(group, &t, NULL) && t == term) {
        while (i < nfiles && njobs < pk.segments_per_pass && njobs < PK_SEGMENTS_MAX) {
            uint32_t j = i, used = EFS_PACK_HEADER_BYTES + EFS_HASH_SIZE;
            while (j < nfiles && j - i < EFS_PACK_MAX_RECORDS &&
                   used + EFS_PACK_RECORD_BYTES + files[j].c.size <= EFS_SEGMENT_MAX_SIZE) {
                used += EFS_PACK_RECORD_BYTES + (uint32_t)files[j].c.size;
                j++;
            }
            /* Budget: build the segment only when the bucket holds its files. */
            if (pk.g[pk_gi(group)].tokens < (double)(j - i)) { STAT_ADD(budget_waits, 1); break; }
            pk.g[pk_gi(group)].tokens -= (double)(j - i);
            jobs[njobs++] = (struct pk_job){.start = i, .end = j};
            i = j;
        }
    }
    if (njobs) {
        uint32_t first = jobs[0].start, span = jobs[njobs - 1].end - first;
        int *rcs = malloc(span * sizeof(*rcs));
        uint64_t t0 = pk_now_ns();
        if (rcs) pk_read_files(nodes, count, files + first, span, flen, rcs);
        /* A file that cannot be read stays a candidate. */
        for (uint32_t q = 0; q < njobs; q++) {
            uint32_t k = jobs[q].start;
            for (uint32_t x = jobs[q].start; x < jobs[q].end; x++) {
                if (!rcs || rcs[x - first] != EFS_OK) {
                    STAT_ADD(read_errors, 1);
                    free(files[x].data);
                    files[x].data = NULL;
                    continue;
                }
                if (k != x) { struct pk_file tmp = files[k]; files[k] = files[x]; files[x] = tmp; }
                k++;
            }
            jobs[q].n = k - jobs[q].start;
        }
        free(rcs);
        uint64_t t1 = pk_now_ns();
        t_read = t1 - t0;
        pthread_t th[PK_SEGMENTS_MAX];
        int started[PK_SEGMENTS_MAX] = {0};
        for (uint32_t q = 0; q < njobs; q++) {
            jobs[q] = (struct pk_job){.start = jobs[q].start, .end = jobs[q].end, .n = jobs[q].n,
                .group = group, .term = term, .nodes = nodes, .count = count, .files = files + jobs[q].start,
                .done = drop + PK_SCAN_MAX + (size_t)q * EFS_PACK_MAX_RECORDS};
            if (!jobs[q].n) continue;
            started[q] = q > 0 && pthread_create(&th[q], NULL, pk_job_run, &jobs[q]) == 0;
            if (q > 0 && !started[q]) pk_job_run(&jobs[q]);
        }
        if (jobs[0].n) pk_job_run(&jobs[0]);
        for (uint32_t q = 0; q < njobs; q++) {
            if (started[q]) pthread_join(th[q], NULL);
            if (!jobs[q].n) continue;
            segs++;
            for (uint32_t d = 0; d < jobs[q].ndone && ndrop < PK_SCAN_MAX; d++) drop[ndrop++] = jobs[q].done[d];
        }
        t_seg = pk_now_ns() - t1;
    }
    more = i < nfiles && segs >= pk.segments_per_pass &&
           pk.g[pk_gi(group)].tokens >= (double)EFS_PACK_MAX_RECORDS / 4;
    pk_unindex(group, drop, ndrop);
    /* Compaction is scheduled maintenance, decoupled from foreground I/O. */
    {
        uint32_t want = pk_compact_due(pk.g[pk_gi(group)].busy, pk_now_ns(), &pk.g[pk_gi(group)].idle_since_ns,
                                       &pk.g[pk_gi(group)].compact_batch_ns, pk_minute_of_day());
        for (uint32_t q = 0; q < want && !__atomic_load_n(&pk.stop, __ATOMIC_ACQUIRE); q++)
            if (!pk_compact(kv, group, term, nodes, count)) break;
    }
    if (segs || ndrop)
        fprintf(stderr, "packer: group=%u term=%llu busy=%d fg_commits_per_s=%.0f lag_s=%.0f rate=%.0f pressure=%.2f unpacked_gib=%.1f used_pct=%.1f candidates=%u eligible=%u segments=%u unindexed=%u packed_total=%llu scan_ms=%llu read_ms=%llu seg_ms=%llu pass_ms=%llu\n",
                group, (unsigned long long)term, pk.g[pk_gi(group)].busy, pk.g[pk_gi(group)].fg_rate,
                (double)pk.g[pk_gi(group)].lag_ns / 1e9, pk.g[pk_gi(group)].rate, pk.g[pk_gi(group)].pressure,
                (double)(pk.g[pk_gi(group)].backlog * (uint64_t)EFS_FRAGMENT_SIZE) / (double)(1ull << 30),
                pk.g[pk_gi(group)].used_frac * 100.0,
                sc.n, nfiles, segs, ndrop,
                (unsigned long long)__atomic_load_n(&pk.st.files_packed, __ATOMIC_RELAXED),
                (unsigned long long)((t_pass - t_scan) / 1000000ull),
                (unsigned long long)(t_read / 1000000ull), (unsigned long long)(t_seg / 1000000ull),
                (unsigned long long)((pk_now_ns() - t_pass) / 1000000ull));
out:
    if (files) for (uint32_t x = 0; x < PK_SCAN_MAX; x++) free(files[x].data);
    free(files);
    free(sc.e);
    free(drop);
    return more;
}

/* Space pressure in [0, 1]: the larger of the unpacked-bytes and quota
 * signals, each linear from its start to its full point. */
static double pk_pressure(uint64_t unpacked_bytes, double used_frac)
{
    double pb = 0, ps = 0;
    if (pk.unpacked_start && unpacked_bytes > pk.unpacked_start)
        pb = (double)(unpacked_bytes - pk.unpacked_start) / (double)pk.unpacked_start;
    if (pk.space_full > pk.space_start && used_frac > pk.space_start)
        ps = (used_frac - pk.space_start) / (pk.space_full - pk.space_start);
    else if (pk.space_full <= pk.space_start && used_frac >= pk.space_full)
        ps = 1;
    double p = pb > ps ? pb : ps;
    return p > 1 ? 1 : p < 0 ? 0 : p;
}
static double pk_busy_rate(double pressure)
{
    if (pk.busy_rate >= pk.idle_rate) return pk.busy_rate;
    return pk.busy_rate + (pk.idle_rate - pk.busy_rate) * pressure;
}
/* Highest quota usage among the group's voters (the local row is exact,
 * peers' rows come from membership gossip). */
static double pk_used_frac(uint8_t group)
{
    uint32_t voters = server_raft_host_group_voters(group);
    double m = 0;
    pthread_mutex_lock(&pk.s->lock);
    for (uint32_t i = 0; i < pk.s->node_count; i++) {
        const struct efs_node *n = &pk.s->nodes[i];
        if (!n->id || n->id > EFS_MAX_NODES || !(voters & (1u << (n->id - 1))) || !n->quota) continue;
        double f = (double)n->used / (double)n->quota;
        if (f > m) m = f;
    }
    pthread_mutex_unlock(&pk.s->lock);
    return m;
}

static void *pk_main(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&pk.stop, __ATOMIC_ACQUIRE)) {
        static const uint8_t groups[2] = {EFS_RAFT_GROUP_SHARD, EFS_RAFT_GROUP_SHARD2};
        /* Foreground load over all hosted groups: commits in the last
         * interval that were not the packer's own proposals. */
        uint64_t now = pk_now_ns();
        /* Commit rates over windows of at least a second: passes can run
         * back to back, and a rate over a few milliseconds is noise. */
        static double fg_total;
        static int measured;
        for (int g = 0; g < 2; g++) {
            uint64_t commit = server_raft_host_group_commit(groups[g]);
            if (pk.g[g].last_ns && now < pk.g[g].last_ns + 1000000000ull && commit >= pk.g[g].last_commit)
                continue;
            uint64_t own = __atomic_exchange_n(&pk.g[g].own, 0, __ATOMIC_RELAXED);
            if (pk.g[g].last_ns && now > pk.g[g].last_ns && commit >= pk.g[g].last_commit) {
                double w = (double)(now - pk.g[g].last_ns) / 1e9;
                double fg = (double)(commit - pk.g[g].last_commit) - (double)own;
                pk.g[g].fg_rate = fg > 0 ? fg / w : 0;
                measured = 1;
            }
            pk.g[g].last_ns = commit ? now : 0;
            pk.g[g].last_commit = commit;
        }
        fg_total = pk.g[0].fg_rate + pk.g[1].fg_rate;
        double dt = 0;
        static uint64_t last_acc;
        if (last_acc && now > last_acc) dt = (double)(now - last_acc) / 1e9;
        last_acc = now;
        int busy = fg_total > pk.busy_commits, more = 0;
        if (measured && busy) STAT_ADD(busy_passes, 1);
        for (int g = 0; g < 2; g++) {
            uint64_t term = 0;
            pk.g[g].busy = busy;
            if (!server_raft_host_pack_leader(groups[g], &term, NULL)) {
                pk.g[g].backlog = 0; /* counted by that group's leader */
                pk.g[g].count_ns = 0;
                continue;
            }
            if (measured) {
                pk.g[g].used_frac = pk_used_frac(groups[g]);
                /* Both groups' unpacked files occupy the same nodes. */
                pk.g[g].pressure = pk_pressure((pk.g[0].backlog + pk.g[1].backlog) * (uint64_t)EFS_FRAGMENT_SIZE,
                                               pk.g[g].used_frac);
                double rate = busy ? pk_busy_rate(pk.g[g].pressure) : pk.idle_rate;
                pk.g[g].rate = rate;
                double cap = rate * 2.0 > 2.0 * EFS_PACK_MAX_RECORDS ? rate * 2.0 : 2.0 * EFS_PACK_MAX_RECORDS;
                pk.g[g].tokens += rate * dt;
                if (pk.g[g].tokens > cap) pk.g[g].tokens = cap;
            }
            more |= pk_group(groups[g], term);
        }
        STAT_ADD(passes, 1);
        /* Backlog and budget left: go straight into the next pass. */
        for (uint64_t w = 0; !more && w < pk.interval_ms && !__atomic_load_n(&pk.stop, __ATOMIC_ACQUIRE); w += 50)
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
    pk.idle_rate = (double)env_u64("EFS_PACK_FILES_PER_S", 5000, 1);
    pk.busy_rate = (double)env_u64("EFS_PACK_BUSY_FILES_PER_S", 50, 1);
    pk.busy_commits = (double)env_u64("EFS_PACK_BUSY_COMMITS_PER_S", 100, 1);
    pk.unpacked_start = env_u64("EFS_PACK_UNPACKED_GIB", 64, 1) << 30;
    pk.space_start = (double)env_u64("EFS_PACK_SPACE_START_PCT", 50, 0) / 100.0;
    pk.space_full = (double)env_u64("EFS_PACK_SPACE_FULL_PCT", 80, 0) / 100.0;
    pk.compact_idle_ns = env_u64("EFS_PACK_COMPACT_IDLE_S", 60, 1) * 1000000000ull;
    pk.compact_idle_segs = (uint32_t)env_u64("EFS_PACK_COMPACT_IDLE_SEGS", 8, 1);
    pk.compact_daily_start = (uint32_t)env_u64("EFS_PACK_COMPACT_DAILY_START_MIN", 180, 0) % 1440;
    pk.compact_daily_min = (uint32_t)env_u64("EFS_PACK_COMPACT_DAILY_MIN", 120, 1);
    if (pk.compact_daily_min > 1440) pk.compact_daily_min = 1440;
    pk.compact_cold_ns = env_u64("EFS_PACK_COMPACT_COLD_S", 86400, 1) * 1000000000ull;
    pk.backfill_shards = (uint32_t)env_u64("EFS_PACK_BACKFILL_SHARDS", 64, 1);
    if (pthread_create(&pk.tid, NULL, pk_main, NULL)) return -1;
    pk.started = 1;
    fprintf(stderr, "packer: started min_age_ms=%llu interval_ms=%llu segments_per_pass=%u "
            "files_per_s=%.0f busy_files_per_s=%.0f busy_commits_per_s=%.0f unpacked_gib=%llu space_pct=%.0f-%.0f "
            "compact_idle_s=%llu compact_idle_segs=%u compact_daily=%02u:%02u+%umin compact_cold_s=%llu\n",
            (unsigned long long)(pk.min_age_ns / 1000000ull), (unsigned long long)pk.interval_ms,
            pk.segments_per_pass, pk.idle_rate, pk.busy_rate, pk.busy_commits,
            (unsigned long long)(pk.unpacked_start >> 30), pk.space_start * 100, pk.space_full * 100,
            (unsigned long long)(pk.compact_idle_ns / 1000000000ull), pk.compact_idle_segs,
            pk.compact_daily_start / 60, pk.compact_daily_start % 60, pk.compact_daily_min,
            (unsigned long long)(pk.compact_cold_ns / 1000000000ull));
    return 0;
}

void server_packer_stop(void)
{
    if (!pk.started) return;
    __atomic_store_n(&pk.stop, 1, __ATOMIC_RELEASE);
    pthread_join(pk.tid, NULL);
    pk.started = 0;
}
