/* K108: the snapshot import holds two read windows and one batch, not the
 * images. A 24 MiB incoming image against a 24 MiB local image with a
 * 7000-key diff (more than six batches) is imported while malloc/free are
 * counted (-Wl,--wrap in the Makefile); the live heap the importer reaches
 * must stay under 2 MiB, and the KV must end with exactly the incoming
 * group keys. Also: an import cut off between batches (a restart) is redone
 * from a fresh local image and converges; a batch the KV refused stays pending
 * and a further step applies it; a malformed or unsorted image is refused before any write. */
#include "efs/common.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/kv_snap.h"
#include "efs/raft.h"
#include "test_tmpdir.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(c, m)                                                           \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, m);       \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* ---- live-heap counter (only inside a tracked window) ---- */
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
static int track;
static long long live, peak;
static void heap_add(long long d)
{
    live += d;
    if (live > peak)
        peak = live;
}
void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);

    if (track && p)
        heap_add((long long)malloc_usable_size(p));
    return p;
}
void *__wrap_calloc(size_t a, size_t b)
{
    void *p = __real_calloc(a, b);

    if (track && p)
        heap_add((long long)malloc_usable_size(p));
    return p;
}
void *__wrap_realloc(void *o, size_t n)
{
    long long was = track && o ? (long long)malloc_usable_size(o) : 0;
    void *p = __real_realloc(o, n);

    if (track && (p || !n))
        heap_add(-was + (p ? (long long)malloc_usable_size(p) : 0));
    return p;
}
void __wrap_free(void *p)
{
    if (track && p)
        heap_add(-(long long)malloc_usable_size(p));
    __real_free(p);
}

/* ---- the KV under import: a mem kv behind a sink that counts batches and
 * can fail one. The mem kv's own growth is not the importer's heap. ---- */
struct sink {
    struct efs_kv *inner;
    int calls, fail_at, failed;
};
static int sink_batch(void *ctx, const struct efs_kv_item *it, uint32_t n)
{
    struct sink *s = ctx;
    int was = track, rc;

    s->calls++;
    if (s->calls == s->fail_at) {
        s->failed++;
        return EFS_ERR_IO;
    }
    track = 0;
    rc = efs_kv_batch(s->inner, it, n);
    track = was;
    return rc;
}
static const struct efs_kv_ops sink_ops = {.batch = sink_batch};

#define VLEN 4096u
#define N_IN 6000u          /* incoming: inodes 1..6000 */
#define LO_FIRST 3001u      /* local: inodes 3001..9000 */
#define LO_LAST 9000u
#define BOUND (2u << 20)

static void ikey(uint32_t shard, uint32_t i, uint8_t *k, uint32_t *kl)
{
    CHECK(efs_kv_key_inode(shard, i, k, kl) == EFS_OK, "key");
}
static void ival(uint8_t *v, uint32_t i, uint32_t variant)
{
    for (uint32_t j = 0; j < VLEN; j++)
        v[j] = (uint8_t)(i * 31u + variant * 7u + j);
}
static void put_le32(FILE *f, uint32_t x)
{
    uint8_t b[4] = {(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16),
                    (uint8_t)(x >> 24)};
    fwrite(b, 1, 4, f);
}
static void put_rec(FILE *f, const uint8_t *k, uint32_t kl, const uint8_t *v,
                    uint32_t vl)
{
    fputc(1, f);
    put_le32(f, kl);
    put_le32(f, vl);
    fwrite(k, 1, kl, f);
    fwrite(v, 1, vl, f);
}

struct dump {
    uint8_t group;
    uint32_t n, cap;
    uint8_t **k, **v;
    uint32_t *kl, *vl;
    uint32_t bad;
};
static int dump_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v,
                   uint32_t vl)
{
    struct dump *d = u;

    uint32_t shard = kl < 2 ? 0xffffu : ((uint32_t)k[0] << 8) | k[1];

    if (shard > 4095 || ((shard & 1u) ? 0 : 2) != d->group)
        return 0;
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 1024;
        d->k = realloc(d->k, d->cap * sizeof(*d->k));
        d->v = realloc(d->v, d->cap * sizeof(*d->v));
        d->kl = realloc(d->kl, d->cap * sizeof(*d->kl));
        d->vl = realloc(d->vl, d->cap * sizeof(*d->vl));
    }
    d->k[d->n] = malloc(kl);
    d->v[d->n] = malloc(vl ? vl : 1);
    memcpy(d->k[d->n], k, kl);
    memcpy(d->v[d->n], v, vl);
    d->kl[d->n] = kl;
    d->vl[d->n] = vl;
    d->n++;
    return 0;
}
static int dump_cmp(const void *a, const void *b, void *arg)
{
    struct dump *d = arg;
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    uint32_t n = d->kl[x] < d->kl[y] ? d->kl[x] : d->kl[y];
    int c = memcmp(d->k[x], d->k[y], n);

    return c ? c : d->kl[x] < d->kl[y] ? -1 : d->kl[x] > d->kl[y];
}
/* Write the group-0 keys of kv as an image file, sorted (the local export). */
static void write_local(struct efs_kv *kv, const char *path)
{
    struct dump d;
    uint32_t *ord;
    FILE *f = fopen(path, "wb");

    memset(&d, 0, sizeof(d));
    CHECK(f != NULL, "open local image");
    CHECK(efs_kv_scan(kv, dump_cb, &d) == EFS_OK, "scan");
    ord = malloc((d.n ? d.n : 1) * sizeof(*ord));
    for (uint32_t i = 0; i < d.n; i++)
        ord[i] = i;
    qsort_r(ord, d.n, sizeof(*ord), dump_cmp, &d);
    put_le32(f, d.n);
    for (uint32_t i = 0; i < d.n; i++)
        put_rec(f, d.k[ord[i]], d.kl[ord[i]], d.v[ord[i]], d.vl[ord[i]]);
    fclose(f);
    for (uint32_t i = 0; i < d.n; i++) {
        free(d.k[i]);
        free(d.v[i]);
    }
    free(d.k);
    free(d.v);
    free(d.kl);
    free(d.vl);
    free(ord);
}
/* inodes first..last, variant 0, as an image file */
static void write_incoming(const char *path, uint32_t first, uint32_t last,
                           int swap_two)
{
    FILE *f = fopen(path, "wb");
    uint8_t k[64], v[VLEN];
    uint32_t kl;

    CHECK(f != NULL, "open incoming image");
    put_le32(f, last - first + 1);
    for (uint32_t i = first; i <= last; i++) {
        uint32_t ino = i;

        if (swap_two && (i == first + 10 || i == first + 11))
            ino = i == first + 10 ? first + 11 : first + 10;
        ikey(1, ino, k, &kl);
        ival(v, ino, 0);
        put_rec(f, k, kl, v, VLEN);
    }
    fclose(f);
}

struct world {
    char dir[EFS_TEST_PATH], in[EFS_TEST_PATH + 16], loc[EFS_TEST_PATH + 16];
    struct efs_kv *mem;
    struct sink s;
    struct efs_kv kv;
};
static void world_make(struct world *w, int fail_at)
{
    uint8_t k[64], v[VLEN];
    uint32_t kl;

    memset(w, 0, sizeof(*w));
    efs_test_path(w->dir, sizeof(w->dir), "efs-snapimp-XXXXXX");
    CHECK(efs_test_mkdtemp(w->dir) != NULL, "tmpdir");
    snprintf(w->in, sizeof(w->in), "%s/in.kvx", w->dir);
    snprintf(w->loc, sizeof(w->loc), "%s/local.kvx", w->dir);
    w->mem = efs_kv_mem_create();
    CHECK(w->mem != NULL, "mem kv");
    for (uint32_t i = LO_FIRST; i <= LO_LAST; i++) {
        ikey(1, i, k, &kl);
        ival(v, i, i % 3 == 0 ? 1 : 0);
        CHECK(efs_kv_put(w->mem, k, kl, v, VLEN) == EFS_OK, "put local");
    }
    ikey(2, 77, k, &kl);   /* the other group's key: must survive */
    CHECK(efs_kv_put(w->mem, k, kl, (const uint8_t *)"g2", 2) == EFS_OK, "g2");
    {
        const uint8_t cur[] = {255, 255, 'r', 'a', 'f', 't', 0};

        CHECK(efs_kv_put(w->mem, cur, sizeof(cur), (const uint8_t *)"cur", 3) ==
                  EFS_OK, "cursor");
    }
    write_incoming(w->in, 1, N_IN, 0);
    write_local(w->mem, w->loc);
    w->s.inner = w->mem;
    w->s.fail_at = fail_at;
    w->kv.ops = &sink_ops;
    w->kv.ctx = &w->s;
}
static void world_free(struct world *w)
{
    efs_kv_destroy(w->mem);
    free(w->mem);   /* efs_kv_destroy frees the store, not the handle */
}

struct chk {
    uint32_t n, wrong;
};
static int chk_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v,
                  uint32_t vl)
{
    struct chk *c = u;
    uint8_t want[VLEN];
    uint32_t shard = ((uint32_t)k[0] << 8) | k[1];

    if (shard != 1)
        return 0;
    c->n++;
    if (kl != 11 || vl != VLEN) {
        c->wrong++;
        return 0;
    }
    {
        uint64_t ino = 0;
        for (int j = 0; j < 8; j++)
            ino = ino << 8 | k[3 + j];
        ival(want, (uint32_t)ino, 0);
        if (ino < 1 || ino > N_IN || memcmp(want, v, VLEN) != 0)
            c->wrong++;
    }
    return 0;
}
/* Exactly the incoming group-0 keys; group 2 and the node-local cursor kept. */
static int converged(struct world *w)
{
    struct chk c = {0, 0};
    uint8_t k[64], buf[16];
    uint32_t kl, bl = sizeof(buf);
    const uint8_t cur[] = {255, 255, 'r', 'a', 'f', 't', 0};

    efs_kv_scan(w->mem, chk_cb, &c);
    ikey(2, 77, k, &kl);
    if (efs_kv_get(w->mem, k, kl, buf, &bl) != EFS_OK || bl != 2)
        return 0;
    bl = sizeof(buf);
    if (efs_kv_get(w->mem, cur, sizeof(cur), buf, &bl) != EFS_OK || bl != 3)
        return 0;
    return c.n == N_IN && c.wrong == 0;
}

static int run_all(struct efs_kv_import *im, int *steps, int *errs)
{
    int done = 0;

    while (!done) {
        int rc = efs_kv_import_step(im, &done);

        if (rc != EFS_OK) {
            (*errs)++;
            if (*errs > 3)
                return rc;
            continue;
        }
        (*steps)++;
        if (*steps > 100000)
            return EFS_ERR_BUSY;
    }
    return EFS_OK;
}

static void test_bounded(void)
{
    struct world w;
    struct efs_kv_import *im = NULL;
    uint64_t sp = 0, nd = 0;
    uint32_t nin = 0, nloc = 0;
    int steps = 0, errs = 0, rc;

    world_make(&w, 0);
    live = peak = 0;
    track = 1;
    rc = efs_kv_import_open(&w.kv, EFS_RAFT_GROUP_SHARD, w.in, w.loc, &im);
    /* The host unlinks both files once open returned; the fds stay valid. */
    unlink(w.in);
    unlink(w.loc);
    CHECK(rc == EFS_OK && im, "open");
    if (rc == EFS_OK)
        rc = run_all(im, &steps, &errs);
    efs_kv_import_stat(im, &nin, &nloc, &sp, &nd);
    efs_kv_import_close(im);
    track = 0;
    CHECK(rc == EFS_OK, "import completes");
    CHECK(nin == N_IN && nloc == LO_LAST - LO_FIRST + 1, "image counts");
    CHECK(nd == 7000, "diff is 3000 new + 1000 changed + 3000 deleted");
    CHECK(steps > 6, "diff larger than one batch takes several steps");
    CHECK(sp <= BOUND, "importer's own buffer high-water under 2 MiB");
    CHECK(peak <= (long long)BOUND, "live heap under 2 MiB");
    fprintf(stderr, "[snapimp] image=%u KiB diff=%llu keys steps=%d buf_peak=%llu heap_peak=%lld\n",
            (unsigned)((uint64_t)N_IN * (VLEN + 20) / 1024),
            (unsigned long long)nd, steps, (unsigned long long)sp, peak);
    CHECK(converged(&w), "KV holds exactly the incoming keys");
    world_free(&w);
}

/* A restart between batches leaves a mix; the next import (fresh local
 * image of what is there now) ends with exactly the incoming keys. */
static void test_interrupted(void)
{
    struct world w;
    struct efs_kv_import *im = NULL;
    int steps = 0, errs = 0, done = 0, rc;

    world_make(&w, 0);
    CHECK(efs_kv_import_open(&w.kv, EFS_RAFT_GROUP_SHARD, w.in, w.loc, &im) == EFS_OK,
          "open");
    for (int i = 0; i < 3; i++)
        CHECK(efs_kv_import_step(im, &done) == EFS_OK && !done, "three batches");
    efs_kv_import_close(im);   /* the process ends here */
    CHECK(!converged(&w), "an interrupted import leaves a mix");
    write_local(w.mem, w.loc); /* after restart: export of the KV as it is */
    rc = efs_kv_import_open(&w.kv, EFS_RAFT_GROUP_SHARD, w.in, w.loc, &im);
    CHECK(rc == EFS_OK, "reopen");
    rc = run_all(im, &steps, &errs);
    efs_kv_import_close(im);
    CHECK(rc == EFS_OK && errs == 0, "second import completes");
    CHECK(converged(&w), "after restart the KV is exactly the incoming keys");
    world_free(&w);
}

/* A batch the KV refuses stays pending: a further step applies it, no key is
 * skipped. (The host steps again only on BUSY and discards the importer on
 * any other error: test_snap_import_host.) */
static void test_failed_batch(void)
{
    struct world w;
    struct efs_kv_import *im = NULL;
    int steps = 0, errs = 0, rc;

    world_make(&w, 3);
    CHECK(efs_kv_import_open(&w.kv, EFS_RAFT_GROUP_SHARD, w.in, w.loc, &im) == EFS_OK,
          "open");
    rc = run_all(im, &steps, &errs);
    efs_kv_import_close(im);
    CHECK(rc == EFS_OK && errs == 1 && w.s.failed == 1, "one injected failure");
    CHECK(converged(&w), "retry lands the failed batch");
    world_free(&w);
}

/* Malformed or unsorted images are refused before the KV is touched. */
static void test_refused(void)
{
    struct world w;
    struct efs_kv_import *im = NULL;
    FILE *f;

    world_make(&w, 0);
    write_incoming(w.in, 1, N_IN, 1);   /* two records out of order */
    CHECK(efs_kv_import_open(&w.kv, EFS_RAFT_GROUP_SHARD, w.in, w.loc, &im) ==
              EFS_ERR_PROTO && !im, "unsorted incoming refused");
    write_incoming(w.in, 1, 100, 0);
    f = fopen(w.in, "r+b");
    fseek(f, 0, SEEK_END);
    fputc(0, f);                        /* a byte after the last record */
    fclose(f);
    CHECK(efs_kv_import_open(&w.kv, EFS_RAFT_GROUP_SHARD, w.in, w.loc, &im) ==
              EFS_ERR_PROTO && !im, "trailing byte refused");
    write_incoming(w.in, 1, 100, 0);
    f = fopen(w.in, "r+b");
    fputc(200, f);                      /* count says 200, the file holds 100 */
    fclose(f);
    CHECK(efs_kv_import_open(&w.kv, EFS_RAFT_GROUP_SHARD, w.in, w.loc, &im) ==
              EFS_ERR_PROTO && !im, "count beyond the file refused");
    CHECK(w.s.calls == 0, "nothing was written");
    world_free(&w);
}

/* ---- small shapes: the images are lists of (inode, value length) ---- */
struct spec {
    uint32_t ino, vlen;
};
static void sval(uint8_t *v, uint32_t ino, uint32_t vlen, uint32_t variant)
{
    for (uint32_t j = 0; j < vlen; j++)
        v[j] = (uint8_t)(ino * 31u + variant * 7u + j);
}
/* raw image: n records of s[], in the order given, optionally with one
 * record in shard 2 (outside group 0) at position bad_at (or -1) */
static void write_img(const char *path, const struct spec *s, uint32_t n,
                      int bad_at)
{
    FILE *f = fopen(path, "wb");
    uint8_t k[64], v[64];
    uint32_t kl;

    CHECK(f != NULL, "open image");
    put_le32(f, n);
    for (uint32_t i = 0; i < n; i++) {
        ikey(bad_at == (int)i ? 2 : 1, s[i].ino, k, &kl);
        sval(v, s[i].ino, s[i].vlen, 0);
        put_rec(f, k, kl, v, s[i].vlen);
    }
    fclose(f);
}
static void put_specs(struct efs_kv *kv, const struct spec *s, uint32_t n,
                      uint32_t variant)
{
    uint8_t k[64], v[64];
    uint32_t kl;

    for (uint32_t i = 0; i < n; i++) {
        ikey(1, s[i].ino, k, &kl);
        sval(v, s[i].ino, s[i].vlen, variant);
        CHECK(efs_kv_put(kv, k, kl, v, s[i].vlen) == EFS_OK, "put");
    }
}
/* The group-0 keys of kv are exactly s[] with the incoming values. */
struct want {
    const struct spec *s;
    uint32_t n, seen, wrong;
};
static int want_cb(void *u, const uint8_t *k, uint32_t kl, const uint8_t *v,
                   uint32_t vl)
{
    struct want *w = u;
    uint8_t exp[64];
    uint64_t ino = 0;

    if ((((uint32_t)k[0] << 8) | k[1]) != 1)
        return 0;
    for (int j = 0; kl == 11 && j < 8; j++)
        ino = ino << 8 | k[3 + j];
    if (w->seen >= w->n || kl != 11 || ino != w->s[w->seen].ino ||
        vl != w->s[w->seen].vlen) {
        w->wrong++;
    } else {
        sval(exp, w->s[w->seen].ino, vl, 0);
        if (vl && memcmp(exp, v, vl) != 0)
            w->wrong++;
    }
    w->seen++;
    return 0;
}
static int holds_exactly(struct efs_kv *kv, const struct spec *s, uint32_t n)
{
    struct want w = {s, n, 0, 0};
    uint8_t k[64], buf[8];
    uint32_t kl, bl = sizeof(buf);

    if (efs_kv_scan(kv, want_cb, &w) != EFS_OK)
        return 0;
    ikey(2, 77, k, &kl);        /* another group's key is never touched */
    return w.seen == n && !w.wrong &&
           efs_kv_get(kv, k, kl, buf, &bl) == EFS_OK && bl == 2;
}
/* Import in -> lo on a kv holding lo (+ one group-2 key); true when the kv
 * ends with exactly in. lo_kv: a store to use instead of a mem kv, or NULL. */
static int merge_case(const struct spec *in, uint32_t nin, const struct spec *lo,
                      uint32_t nlo, struct efs_kv *lo_kv, int real_export)
{
    char dir[EFS_TEST_PATH], inp[EFS_TEST_PATH + 16], locp[EFS_TEST_PATH + 16];
    struct efs_kv *kv = lo_kv ? lo_kv : efs_kv_mem_create();
    struct efs_kv_import *im = NULL;
    uint8_t k[64];
    uint32_t kl;
    int done = 0, steps = 0, ok;

    efs_test_path(dir, sizeof(dir), "efs-snapshape-XXXXXX");
    CHECK(efs_test_mkdtemp(dir) != NULL, "tmpdir");
    snprintf(inp, sizeof(inp), "%s/in.kvx", dir);
    snprintf(locp, sizeof(locp), "%s/local.kvx", dir);
    put_specs(kv, lo, nlo, 5);
    ikey(2, 77, k, &kl);
    CHECK(efs_kv_put(kv, k, kl, (const uint8_t *)"g2", 2) == EFS_OK, "g2");
    write_img(inp, in, nin, -1);
    if (real_export) {  /* the exporter the host uses, not the test's own */
        struct efs_kv_lsm_view *view;

        CHECK(efs_kv_lsm_flush(kv) == EFS_OK, "flush");
        CHECK(efs_kv_lsm_view_pin(kv, &view) == EFS_OK, "pin");
        CHECK(efs_kv_lsm_view_export(view, EFS_RAFT_GROUP_SHARD, locp) == EFS_OK,
              "export");
        efs_kv_lsm_view_unpin(view);
    } else {
        write_local(kv, locp);
    }
    CHECK(efs_kv_import_open(kv, EFS_RAFT_GROUP_SHARD, inp, locp, &im) == EFS_OK,
          "open");
    while (!done && steps++ < 100)
        CHECK(efs_kv_import_step(im, &done) == EFS_OK, "step");
    efs_kv_import_close(im);
    ok = done && holds_exactly(kv, in, nin);
    if (lo_kv) {
        efs_kv_lsm_close(kv);
    } else {
        efs_kv_destroy(kv);
        free(kv);
    }
    return ok;
}

static void test_shapes(void)
{
    static const struct spec e[1] = {{0, 0}};
    static const struct spec a[] = {{1, 8}, {2, 8}, {3, 8}};
    static const struct spec hi[] = {{5, 8}, {6, 8}, {7, 8}, {8, 8}};
    static const struct spec lo[] = {{1, 8}, {2, 8}};
    static const struct spec b[] = {{2, 8}, {3, 8}};
    static const struct spec one[] = {{1, 8}};
    static const struct spec zin[] = {{1, 0}, {2, 0}, {3, 5}};
    static const struct spec zlo[] = {{2, 7}, {3, 0}, {4, 0}};

    CHECK(merge_case(e, 0, e, 0, NULL, 0), "both empty");
    CHECK(merge_case(e, 0, a, 3, NULL, 0), "empty incoming deletes every local key");
    CHECK(merge_case(a, 3, e, 0, NULL, 0), "empty local takes every incoming key");
    CHECK(merge_case(hi, 4, lo, 2, NULL, 0), "local ends before incoming starts");
    CHECK(merge_case(lo, 2, hi, 4, NULL, 0), "incoming ends before local starts");
    CHECK(merge_case(one, 1, b, 2, NULL, 0), "incoming exhausted first");
    CHECK(merge_case(b, 2, one, 1, NULL, 0), "local exhausted first");
    CHECK(merge_case(zin, 3, zlo, 3, NULL, 0),
          "zero-length values: kept, written over, written empty, deleted");
    {
        char dir[EFS_TEST_PATH];
        struct efs_kv *lsm;

        efs_test_path(dir, sizeof(dir), "efs-snaplsm-XXXXXX");
        CHECK(efs_test_mkdtemp(dir) != NULL, "tmpdir");
        lsm = efs_kv_lsm_open(dir, NULL);
        CHECK(lsm != NULL, "open lsm");
        CHECK(merge_case(zin, 3, zlo, 3, lsm, 1),
              "real LSM export and apply: zero-length values");
        efs_test_path(dir, sizeof(dir), "efs-snaplsm-XXXXXX");
        CHECK(efs_test_mkdtemp(dir) != NULL, "tmpdir");
        lsm = efs_kv_lsm_open(dir, NULL);
        CHECK(lsm != NULL, "open lsm");
        CHECK(merge_case(hi, 4, a, 3, lsm, 1), "real LSM export and apply: disjoint");
    }
}

/* Every refusal happens at open, before any write; no importer is returned. */
static void refuse_case(const struct spec *in, uint32_t nin, const struct spec *lo,
                        uint32_t nlo, int bad_in_at, int bad_lo_at, int cut,
                        const char *why)
{
    char dir[EFS_TEST_PATH], inp[EFS_TEST_PATH + 16], locp[EFS_TEST_PATH + 16];
    struct efs_kv_import *im = NULL;
    struct efs_kv *kv = efs_kv_mem_create();
    off_t sz;

    efs_test_path(dir, sizeof(dir), "efs-snaprefuse-XXXXXX");
    CHECK(efs_test_mkdtemp(dir) != NULL, "tmpdir");
    snprintf(inp, sizeof(inp), "%s/in.kvx", dir);
    snprintf(locp, sizeof(locp), "%s/local.kvx", dir);
    write_img(inp, in, nin, bad_in_at);
    write_img(locp, lo, nlo, bad_lo_at);
    if (cut) {
        FILE *f = fopen(inp, "rb");

        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fclose(f);
        CHECK(truncate(inp, sz - 3) == 0, "truncate");
    }
    CHECK(efs_kv_import_open(kv, EFS_RAFT_GROUP_SHARD, inp, locp, &im) ==
              EFS_ERR_PROTO && !im, why);
    efs_kv_destroy(kv);
    free(kv);
}

static void test_refused_shapes(void)
{
    static const struct spec dup[] = {{1, 8}, {2, 8}, {2, 8}, {3, 8}};
    static const struct spec ok[] = {{1, 8}, {2, 8}, {3, 8}};
    static const struct spec down[] = {{3, 8}, {2, 8}};

    refuse_case(dup, 4, ok, 3, -1, -1, 0, "duplicate key in the incoming image");
    refuse_case(ok, 3, dup, 4, -1, -1, 0, "duplicate key in the local image");
    refuse_case(ok, 3, down, 2, -1, -1, 0, "unsorted local image");
    refuse_case(ok, 3, ok, 3, -1, -1, 1, "truncated last record");
    refuse_case(ok, 3, ok, 3, 1, -1, 0, "incoming key outside the group");
    refuse_case(ok, 3, ok, 3, -1, 2, 0, "local key outside the group");
}

int main(void)
{
    test_bounded();
    test_interrupted();
    test_failed_batch();
    test_refused();
    test_shapes();
    test_refused_shapes();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_kv_snap_import: ok\n");
    return 0;
}
