/* A packed segment is validated once per image (ledger init, compaction
 * enumeration, packed-base reads), and every reference read through that
 * one validation is still checked: identity, generations, offset, digest
 * and the record's payload bytes. Also: the abandon sweep is bounded per
 * call and still reaches every object, on the memory store (callback
 * stops) and on an LSM store whose tombstones charge the work budget
 * (W150: work-bound stop and resume, mutation mid-sweep, term change,
 * wraparound, a stop on the last key; no actionable key skipped). */
#include "efs/pack_mapping.h"
#include "efs/checksum.h"
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "efs/raft.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define N EFS_PACK_MAX_RECORDS
static struct efs_pack_record records[N];
static struct efs_pack_ref refs[N];
static uint8_t payload[N][8];

static void put32(uint8_t *p, uint32_t n) { for (int i = 3; i >= 0; i--) { p[i] = (uint8_t)n; n >>= 8; } }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void resign(uint8_t *image)
{
    efs_hash(image, 64, image + 64);
    uint32_t used = get32(image + 20);
    efs_hash(image, used - EFS_HASH_SIZE, image + used - EFS_HASH_SIZE);
}
static size_t build(uint8_t *image, unsigned count, uint64_t id)
{
    size_t geometry;
    for (unsigned i = 0; i < count; i++) {
        memset(payload[i], (int)(i * 7 + 1), sizeof(payload[i]));
        records[i] = (struct efs_pack_record){.ino = 1000 + i, .inode_generation = 3, .content_epoch = 5,
            .record_generation = 7 + i, .len = sizeof(payload[i]), .data = payload[i]};
    }
    assert(!efs_pack_build(id, 1, EFS_RAFT_GROUP_SHARD, records, count, image, EFS_SEGMENT_MAX_SIZE,
                           &geometry, refs));
    return geometry;
}
static void read_refused(const struct efs_pack_view *v, const struct efs_pack_ref *r)
{
    const uint8_t *data = (const uint8_t *)1;
    uint32_t len = 99;
    assert(efs_pack_view_read(v, r, &data, &len) != EFS_OK && !data && !len);
}

static void validate_once(void)
{
    uint8_t *image = malloc(EFS_SEGMENT_MAX_SIZE), *copy = malloc(EFS_SEGMENT_MAX_SIZE);
    assert(image && copy);
    size_t g = build(image, N, 41);
    struct efs_opid owner = {.client_uuid = {9}, .session_epoch = 2, .seq = 41};
    struct efs_pack_ledger *ledger = malloc(sizeof(*ledger));
    assert(ledger);
    /* Ledger init: one whole-image validation for N references (was N+1). */
    uint64_t before = efs_pack_validations();
    assert(!efs_pack_ledger_init(image, g, refs, N, &owner, 7, 0, ledger));
    assert(efs_pack_validations() - before == 1);
    /* Compaction's enumeration: one validation, then every record. */
    struct efs_pack_view v;
    before = efs_pack_validations();
    assert(!efs_pack_view_open(image, g, &v) && v.count == N);
    for (uint32_t i = 0; i < v.count; i++) {
        struct efs_pack_ref r;
        uint8_t a[EFS_PACK_REF_BYTES], b[EFS_PACK_REF_BYTES];
        assert(!efs_pack_view_record_at(&v, i, &r));
        assert(!efs_pack_ref_encode(&r, a) && !efs_pack_ref_encode(&refs[i], b) && !memcmp(a, b, sizeof(a)));
    }
    /* Packed bases sharing the segment: reads add no validation. */
    for (uint32_t i = 0; i < N; i++) {
        const uint8_t *data;
        uint32_t len;
        assert(!efs_pack_view_read(&v, &refs[i], &data, &len));
        assert(len == sizeof(payload[i]) && !memcmp(data, payload[i], len));
    }
    assert(efs_pack_validations() - before == 1);

    /* Wrong references are refused through the view. */
    struct efs_pack_ref bad;
    bad = refs[3]; bad.digest[5] ^= 1; read_refused(&v, &bad);
    bad = refs[3]; bad.inode_generation++; read_refused(&v, &bad);
    bad = refs[3]; bad.record_generation++; read_refused(&v, &bad);
    bad = refs[3]; bad.content_epoch++; read_refused(&v, &bad);
    bad = refs[3]; bad.segment_generation++; read_refused(&v, &bad);
    bad = refs[3]; bad.offset = refs[4].offset; read_refused(&v, &bad);   /* another record */
    bad = refs[3]; bad.offset++; read_refused(&v, &bad);                  /* no record */
    bad = refs[3]; bad.ino = refs[4].ino; read_refused(&v, &bad);
    bad = refs[3]; bad.len--; read_refused(&v, &bad);
    /* ... and by ledger init, which must not admit them. */
    struct efs_pack_ref keep = refs[3];
    refs[3].digest[0] ^= 1;
    assert(efs_pack_ledger_init(image, g, refs, N, &owner, 7, 0, ledger) == EFS_ERR_INVAL);
    refs[3] = keep; refs[3].record_generation++;
    assert(efs_pack_ledger_init(image, g, refs, N, &owner, 7, 0, ledger) == EFS_ERR_INVAL);
    refs[3] = keep; refs[3].offset = refs[4].offset;
    assert(efs_pack_ledger_init(image, g, refs, N, &owner, 7, 0, ledger) == EFS_ERR_INVAL);
    refs[3] = keep; refs[3].ino = refs[4].ino; /* a duplicate inode among the references */
    assert(efs_pack_ledger_init(image, g, refs, N, &owner, 7, 0, ledger) == EFS_ERR_INVAL);
    refs[3] = keep;

    /* A payload flip: refused at open, and through an already-open view. */
    memcpy(copy, image, g);
    copy[refs[9].offset] ^= 0x40;
    struct efs_pack_view cv;
    assert(efs_pack_view_open(copy, g, &cv) == EFS_ERR_CHECKSUM && !cv.image);
    assert(efs_pack_ledger_init(copy, g, refs, N, &owner, 7, 0, ledger) == EFS_ERR_INVAL);
    memcpy(copy, image, g);
    assert(!efs_pack_view_open(copy, g, &cv));
    copy[refs[9].offset] ^= 0x40;              /* mutated after validation */
    read_refused(&cv, &refs[9]);
    struct efs_pack_ref r;
    assert(efs_pack_view_record_at(&cv, 9, &r) == EFS_ERR_CHECKSUM);
    const uint8_t *data;
    uint32_t len;
    assert(!efs_pack_view_read(&cv, &refs[10], &data, &len)); /* other records stay readable */
    /* A header changed after validation, or a view not made by open. */
    memcpy(copy, image, g);
    assert(!efs_pack_view_open(copy, g, &cv));
    copy[40 + 7] ^= 1;                         /* segment generation */
    read_refused(&cv, &refs[0]);
    assert(efs_pack_view_record_at(&cv, 0, &r) == EFS_ERR_INVAL);
    struct efs_pack_view forged = v;
    forged.image = copy;                       /* retargeted onto another buffer */
    memcpy(copy, image, g);
    read_refused(&forged, &refs[0]);
    memset(&forged, 0, sizeof(forged));
    read_refused(&forged, &refs[0]);
    forged = v; forged.count = 2;
    assert(efs_pack_view_record_at(&forged, 0, &r) == EFS_ERR_INVAL);
    /* Duplicate inode, with valid checksums: refused by the sorted check. */
    memcpy(copy, image, g);
    memcpy(copy + EFS_PACK_HEADER_BYTES + 400 * EFS_PACK_RECORD_BYTES, copy + EFS_PACK_HEADER_BYTES + 17 * EFS_PACK_RECORD_BYTES, 8);
    resign(copy);
    assert(efs_pack_validate(copy, g) == EFS_ERR_PROTO);
    assert(efs_pack_ledger_init(copy, g, refs, N, &owner, 7, 0, ledger) == EFS_ERR_INVAL);
    /* Wrong record count in the header is still refused. */
    memcpy(copy, image, g);
    put32(copy + 24, N - 1); resign(copy);
    assert(efs_pack_view_open(copy, g, &cv) != EFS_OK);
    free(ledger);
    free(image);
    free(copy);
}

/* ---- bounded abandon sweep ---- */
static void put_object(struct efs_kv *kv, uint64_t id, const struct efs_opid *owner, int settle)
{
    uint8_t *image = malloc(EFS_SEGMENT_MAX_SIZE), *wire = malloc(EFS_PACK_OBJECT_MAX_BYTES);
    struct efs_pack_object *o = calloc(1, sizeof(*o));
    assert(image && wire && o);
    size_t g = build(image, 4, id), len;
    assert(!efs_pack_ledger_init(image, g, refs, 4, owner, 7, 0, &o->ledger));
    if (settle) for (unsigned i = 0; i < 4; i++) assert(!efs_pack_ledger_cancel(&o->ledger, &refs[i]));
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) o->nodes[i] = i + 1;
    assert(!efs_pack_object_encode(o, wire, EFS_PACK_OBJECT_MAX_BYTES, &len));
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES];
    assert(!efs_pack_object_key(EFS_RAFT_GROUP_SHARD, id, 1, key));
    assert(!efs_kv_put(kv, key, sizeof(key), wire, (uint32_t)len));
    free(image); free(wire); free(o);
}
static void abandon_sweep(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    assert(kv);
    struct efs_opid me = {.client_uuid = {'E', 'F', 'S'}, .session_epoch = 5, .seq = 1};
    struct efs_opid old = me, other = me;
    old.session_epoch = 4;                     /* this node, previous term */
    other.client_uuid[8] = 1;                  /* another node */
    /* 40 objects; ids 1..30 are ours (in flight) or settled, only the last
     * ten, all beyond the first windows, are abandoned. */
    enum { OBJS = 40, LIMIT = 8 };
    for (uint64_t id = 1; id <= OBJS; id++) {
        if (id <= 30) put_object(kv, id, id % 2 ? &me : &other, id % 2 == 0);
        else put_object(kv, id, id % 3 ? &old : &other, 0);
    }
    /* A corrupt object is refused, never cancelled. */
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES], junk[64] = {1};
    assert(!efs_pack_object_key(EFS_RAFT_GROUP_SHARD, OBJS + 1, 1, key));
    assert(!efs_kv_put(kv, key, sizeof(key), junk, sizeof(junk)));

    struct efs_pack_abandon_scan sc;
    memset(&sc, 0, sizeof(sc));
    uint64_t ids[64], gens[64], found[OBJS + 2] = {0};
    uint32_t n, calls = 0;
    assert(!efs_pack_abandon_scan(kv, EFS_RAFT_GROUP_SHARD, &me, &sc, LIMIT, ids, gens, 64, &n));
    calls++;
    assert(n == 0 && sc.scanned == LIMIT && sc.cursor_len && !sc.wraps); /* first window: none */
    while (!sc.wraps) {
        assert(!efs_pack_abandon_scan(kv, EFS_RAFT_GROUP_SHARD, &me, &sc, LIMIT, ids, gens, 64, &n));
        assert(++calls <= OBJS); /* bounded windows always make progress */
        for (uint32_t i = 0; i < n; i++) { assert(gens[i] == 1 && ids[i] > 30 && ids[i] <= OBJS); found[ids[i]]++; }
    }
    for (uint64_t id = 31; id <= OBJS; id++) assert(found[id] == 1);
    assert(sc.scanned == OBJS + 1 && sc.actionable == 10 && sc.refused == 1 && !sc.cursor_len);
    assert(calls == (OBJS + 1 + LIMIT - 1) / LIMIT + ((OBJS + 1) % LIMIT == 0));
    /* The next call starts the sweep over. */
    assert(!efs_pack_abandon_scan(kv, EFS_RAFT_GROUP_SHARD, &me, &sc, LIMIT, ids, gens, 64, &n));
    assert(n == 0 && sc.scanned == OBJS + 1 + LIMIT);
    /* A full result list stops the window early and resumes after it. */
    memset(&sc, 0, sizeof(sc));
    uint32_t total = 0;
    while (!sc.wraps) {
        assert(!efs_pack_abandon_scan(kv, EFS_RAFT_GROUP_SHARD, &me, &sc, 1000, ids, gens, 3, &n));
        assert(n <= 3);
        total += n;
    }
    assert(total == 10);
    assert(efs_pack_abandon_scan(kv, EFS_RAFT_GROUP_SHARD, &me, &sc, 1, ids, gens, 64, &n) == EFS_ERR_INVAL);
    efs_kv_mem_free(kv);
}

/* ---- work-bounded abandon sweep on an LSM store (W150) ----
 * On an LSM store the budget counts every merged key, tombstones included,
 * so a window can stop with no live key seen. The sweep must still visit
 * every actionable object exactly once per pass, resume strictly after the
 * last consumed key, and start over on wraparound or a term change. */
enum { W_OBJS = 48, W_LIMIT = 8, W_FIRST = 2, W_LAST = W_OBJS + 1, W_MAXID = 63 };
static struct { uint8_t present, settled; struct efs_opid owner; } w_obj[W_MAXID + 1];
static uint64_t w_keys; /* live + tombstone keys under the prefix */
static int w_actionable(uint64_t id, const struct efs_opid *me)
{
    return w_obj[id].present && !w_obj[id].settled &&
        (memcmp(w_obj[id].owner.client_uuid, me->client_uuid, EFS_OPID_UUID_LEN) ||
         w_obj[id].owner.session_epoch != me->session_epoch);
}
static void w_put(struct efs_kv *kv, uint64_t id, const struct efs_opid *owner, int settle)
{
    put_object(kv, id, owner, settle);
    w_obj[id].present = 1; w_obj[id].settled = (uint8_t)settle; w_obj[id].owner = *owner; w_keys++;
}
static void w_del(struct efs_kv *kv, uint64_t id)
{
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES];
    assert(!efs_pack_object_key(EFS_RAFT_GROUP_SHARD, id, 1, key));
    assert(!efs_kv_del(kv, key, sizeof(key)));
    w_obj[id].present = 0; /* the key stays as a tombstone: still work */
}
/* `count` deleted generations of `id`: hidden tombstones the budget charges
 * and the callback never sees. Flushed before the delete so each is a
 * tombstone over a segment row, as after a real cancel. */
static void w_tombstones(struct efs_kv *kv, uint64_t id, unsigned count)
{
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES];
    for (unsigned i = 0; i < count; i++) {
        assert(!efs_pack_object_key(EFS_RAFT_GROUP_SHARD, id, 2 + i, key));
        assert(!efs_kv_put(kv, key, sizeof(key), (const uint8_t *)"x", 1));
    }
    assert(!efs_kv_lsm_flush(kv));
    for (unsigned i = 0; i < count; i++) {
        assert(!efs_pack_object_key(EFS_RAFT_GROUP_SHARD, id, 2 + i, key));
        assert(!efs_kv_del(kv, key, sizeof(key)));
    }
    w_keys += count;
}
struct w_pass { uint32_t calls, work_stops, found[W_MAXID + 1]; };
/* One bounded call; checks the per-call contract and tallies the results. */
static void w_call(struct efs_kv *kv, const struct efs_opid *me, struct efs_pack_abandon_scan *sc,
    uint32_t limit, struct w_pass *ps, const uint8_t *prev, uint32_t prev_len)
{
    uint64_t ids[64], gens[64], wraps = sc->wraps;
    uint32_t n;
    struct efs_kv_scan_stats st;
    efs_kv_lsm_scan_stats(NULL, 1);
    assert(!efs_pack_abandon_scan(kv, EFS_RAFT_GROUP_SHARD, me, sc, limit, ids, gens, 64, &n));
    efs_kv_lsm_scan_stats(&st, 0);
    assert(st.keys <= limit);                     /* the budget counts tombstones */
    if (st.keys == limit && st.tombstones) ps->work_stops++;
    if (sc->wraps == wraps) {                     /* stopped: the cursor strictly advances */
        assert(sc->cursor_len == EFS_PACK_OBJECT_KEY_BYTES);
        assert(!prev_len || memcmp(sc->cursor, prev, prev_len) > 0);
    } else assert(sc->wraps == wraps + 1 && !sc->cursor_len);
    for (uint32_t i = 0; i < n; i++) {
        assert(gens[i] == 1 && ids[i] >= W_FIRST && ids[i] <= W_MAXID);
        assert(w_actionable(ids[i], me));        /* never a foreign-looking settled or own object */
        ps->found[ids[i]]++;
    }
    ps->calls++;
}
/* Calls until the sweep wraps. */
static void w_sweep(struct efs_kv *kv, const struct efs_opid *me, struct efs_pack_abandon_scan *sc,
    uint32_t limit, struct w_pass *ps)
{
    uint64_t wraps = sc->wraps;
    uint8_t prev[EFS_KV_KEY_MAX];
    uint32_t prev_len = sc->cursor_len, before = ps->calls;
    memcpy(prev, sc->cursor, sc->cursor_len);
    while (sc->wraps == wraps) {
        w_call(kv, me, sc, limit, ps, prev, prev_len);
        memcpy(prev, sc->cursor, sc->cursor_len); prev_len = sc->cursor_len;
        assert(ps->calls - before <= w_keys / limit + 2); /* every window consumes `limit` keys */
    }
}
static void w_expect(const struct w_pass *ps, const struct efs_opid *me, uint32_t times)
{
    for (uint64_t id = 1; id <= W_MAXID; id++) assert(ps->found[id] == (w_actionable(id, me) ? times : 0));
}
static uint32_t w_count(const struct efs_opid *me)
{
    uint32_t n = 0;
    for (uint64_t id = 1; id <= W_MAXID; id++) n += w_actionable(id, me) ? 1 : 0;
    return n;
}
static void rmtree(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char p[1024];
    if (!d) return;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(dir);
}
static void abandon_sweep_work_bound(void)
{
    char dir[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof(dir), "%s/efs-pack-abandon-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    assert(mkdtemp(dir));
    struct efs_kv_lsm_cfg cfg = {.sync_mode = EFS_KV_LSM_NOSYNC, .memtable_max = 16 * 1024 * 1024, .l0_max = 64};
    struct efs_kv *kv = efs_kv_lsm_open(dir, &cfg);
    assert(kv);
    struct efs_opid me = {.client_uuid = {'E', 'F', 'S'}, .session_epoch = 5, .seq = 1};
    struct efs_opid old = me, other = me, me2 = me;
    old.session_epoch = 4;                     /* this node, previous term */
    other.client_uuid[8] = 1;                  /* another node */
    me2.session_epoch = 6;                     /* this node after a term change */
    memset(w_obj, 0, sizeof(w_obj)); w_keys = 0;
    /* Tombstones before the first object: the first window sees no live key. */
    w_tombstones(kv, 1, W_LIMIT + 2);
    /* id % 4: 0 ours from the previous term, 1 another node's (both
     * actionable); 2 ours in flight (actionable only after the term
     * change); 3 another node's, settled (never). 0..2*LIMIT tombstones
     * follow each object, so windows stop on live keys and on tombstones. */
    for (uint64_t id = W_FIRST; id <= W_LAST; id++) {
        const struct efs_opid *o = id % 4 == 0 ? &old : id % 4 == 2 ? &me : &other;
        w_put(kv, id, o, id % 4 == 3);
        w_tombstones(kv, id, (unsigned)((id * 7) % (2 * W_LIMIT + 1)));
    }
    w_tombstones(kv, W_LAST + 1, W_LIMIT);     /* and after the last object */
    assert(w_count(&me) == 24 && w_count(&me2) == 36);

    /* 1. One pass: every actionable object once, bounded calls, and at
     * least one window stopped by the budget with tombstones in it. */
    struct efs_pack_abandon_scan sc;
    memset(&sc, 0, sizeof(sc));
    struct w_pass ps;
    memset(&ps, 0, sizeof(ps));
    w_call(kv, &me, &sc, W_LIMIT, &ps, NULL, 0);
    assert(ps.calls == 1 && ps.work_stops == 1 && sc.cursor_len && !sc.scanned); /* tombstones only */
    w_sweep(kv, &me, &sc, W_LIMIT, &ps);
    w_expect(&ps, &me, 1);
    assert(sc.wraps == 1 && !sc.cursor_len && sc.scanned == W_OBJS && sc.actionable == 24 && !sc.refused);
    assert(ps.work_stops > ps.calls / 2 && ps.calls > W_OBJS / W_LIMIT + 1); /* the budget, not the callback, paced it */
    /* 2. Wraparound: the next pass starts over and finds the same set. */
    w_sweep(kv, &me, &sc, W_LIMIT, &ps);
    w_expect(&ps, &me, 2);
    assert(sc.wraps == 2 && sc.scanned == 2 * W_OBJS);
    /* 3. Mutation mid-pass: an actionable object ahead is cancelled, a new
     * one is admitted ahead; the pass reports exactly the current set. */
    memset(&sc, 0, sizeof(sc)); memset(&ps, 0, sizeof(ps));
    w_call(kv, &me, &sc, W_LIMIT, &ps, NULL, 0);
    assert(w_actionable(W_LAST, &me));
    w_del(kv, W_LAST);
    w_put(kv, W_LAST + 2, &old, 0);
    w_sweep(kv, &me, &sc, W_LIMIT, &ps);
    w_expect(&ps, &me, 1);
    assert(w_count(&me) == 24 && ps.found[W_LAST] == 0 && ps.found[W_LAST + 2] == 1);
    /* 4. Term change half way: without the restart the objects this node
     * admitted before the cursor are missed until the next wrap; with the
     * packer's restart (cursor to the start) one pass finds them all. */
    memset(&sc, 0, sizeof(sc)); memset(&ps, 0, sizeof(ps));
    uint32_t half = 0;
    while (half < 12) {
        w_call(kv, &me, &sc, W_LIMIT, &ps, NULL, 0);
        assert(!sc.wraps);
        half = 0;
        for (uint64_t id = 1; id <= W_MAXID; id++) half += ps.found[id];
    }
    uint64_t cursor_id = 0;
    for (int i = 0; i < 8; i++) cursor_id = cursor_id << 8 | sc.cursor[3 + i];
    uint32_t newly_before = 0;
    for (uint64_t id = W_FIRST; id <= cursor_id; id++) newly_before += id % 4 == 2;
    assert(newly_before > 0);
    struct w_pass ps2;
    memset(&ps2, 0, sizeof(ps2));
    w_sweep(kv, &me2, &sc, W_LIMIT, &ps2);
    uint32_t missed = 0;
    for (uint64_t id = W_FIRST; id <= cursor_id; id++) missed += id % 4 == 2 && !ps2.found[id];
    assert(missed == newly_before);           /* continuing the old cursor skips them */
    for (uint64_t id = 1; id <= W_MAXID; id++) assert(ps2.found[id] <= 1);
    sc.cursor_len = 0;                        /* what pk_abandon does on a new term */
    memset(&ps2, 0, sizeof(ps2));
    w_sweep(kv, &me2, &sc, W_LIMIT, &ps2);
    w_expect(&ps2, &me2, 1);
    assert(w_count(&me2) == 36);
    /* 5. The budget edges. The merged key count K (live + tombstones) is
     * measured, not assumed. A budget of K consumes every key and finds no
     * next one: exhaustion, a wrap in one call. A budget of K-1 stops
     * one key short; the last key is object W_LAST + 2 (admitted in step
     * 3 after the trailing tombstones) and actionable, so the stop must
     * not skip it: the next window holds only that key, reports it, and
     * wraps. */
    struct efs_kv_scan_stats st;
    memset(&sc, 0, sizeof(sc)); memset(&ps, 0, sizeof(ps));
    efs_kv_lsm_scan_stats(NULL, 1);
    w_call(kv, &me, &sc, 100000, &ps, NULL, 0);
    efs_kv_lsm_scan_stats(&st, 0);
    uint32_t keys = (uint32_t)st.keys;
    assert(sc.wraps == 1 && keys == w_keys && st.emitted == W_OBJS && st.tombstones == w_keys - W_OBJS);
    w_expect(&ps, &me, 1);
    memset(&sc, 0, sizeof(sc)); memset(&ps, 0, sizeof(ps));
    w_call(kv, &me, &sc, keys, &ps, NULL, 0);
    assert(sc.wraps == 1 && !sc.cursor_len && ps.calls == 1);
    w_expect(&ps, &me, 1);
    memset(&sc, 0, sizeof(sc)); memset(&ps, 0, sizeof(ps));
    w_call(kv, &me, &sc, keys - 1, &ps, NULL, 0);
    assert(ps.work_stops == 1 && sc.cursor_len && !sc.wraps);
    assert(w_actionable(W_LAST + 2, &me) && !ps.found[W_LAST + 2]);
    ps.found[W_LAST + 2] = 1;                 /* all but the last key so far */
    w_expect(&ps, &me, 1);
    ps.found[W_LAST + 2] = 0;
    uint8_t prev[EFS_KV_KEY_MAX];
    uint32_t prev_len = sc.cursor_len;
    memcpy(prev, sc.cursor, prev_len);
    efs_kv_lsm_scan_stats(NULL, 1);
    w_call(kv, &me, &sc, keys - 1, &ps, prev, prev_len);
    efs_kv_lsm_scan_stats(&st, 0);
    assert(st.keys == 1 && st.emitted == 1 && !st.tombstones); /* only the last key */
    assert(sc.wraps == 1 && !sc.cursor_len);
    w_expect(&ps, &me, 1);                    /* the last key found, nothing twice */
    w_call(kv, &me, &sc, keys, &ps, NULL, 0);
    w_expect(&ps, &me, 2);                    /* the next pass starts over */
    efs_kv_lsm_close(kv);
    rmtree(dir);
}

int main(void)
{
    validate_once();
    abandon_sweep();
    abandon_sweep_work_bound();
    puts("packed segment validated once per image; refs, corruption, bounded abandon sweep and LSM work-budget stop/resume PASS");
    return 0;
}
