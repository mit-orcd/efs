/* Task FN / K108 "fails before" control: tests/test_kv_snap_import.c with a
 * shim that puts the devel efs_kv_group_import_prepare (whole images in the
 * heap) behind the new efs_kv_import_* API, so the same bound check runs
 * against devel's code. Build it in a tree at the commit before the fix:
 *   gcc -O1 -g -std=gnu99 -D_GNU_SOURCE -Iinclude -Isrc/common -Isrc/kv -I. \
 *     -o tests/scratch_before tests/scratch_before.c libefs.a -lpthread -lm -ldl -libverbs \
 *     -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free
 * Expected on devel: "live heap under 2 MiB" FAILS (heap_peak 50,357,792 B)
 * and "unsorted incoming refused" FAILS (devel sorts instead of refusing); the
 * interrupted-import and failed-batch cases pass there, because the crash
 * semantics did not change. Not part of any build. */
/* K108: the snapshot import holds two read windows and one batch, not the
 * images. A 24 MiB incoming image against a 24 MiB local image with a
 * 7000-key diff (more than six batches) is imported while malloc/free are
 * counted (-Wl,--wrap in the Makefile); the live heap the importer reaches
 * must stay under 2 MiB, and the KV must end with exactly the incoming
 * group keys. Also: an import cut off between batches (a restart) is redone
 * from a fresh local image and converges; a failed batch is applied again by
 * the next step; a malformed or unsorted image is refused before any write. */
#include "efs/common.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
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
/* ---- scratch shim: the devel prepare/apply behind the new API ---- */
struct efs_kv_import { struct efs_kv *kv; struct efs_kv_item *d; uint32_t n, at; uint8_t *a, *b; };
int efs_kv_group_import_prepare(struct efs_kv *, uint8_t, const char *, const char *, struct efs_kv_item **, uint32_t *, uint8_t **, uint8_t **);
int efs_kv_import_open(struct efs_kv *kv, uint8_t g, const char *in, const char *loc, struct efs_kv_import **out)
{ struct efs_kv_import *im = calloc(1, sizeof(*im)); int rc; im->kv = kv;
  rc = efs_kv_group_import_prepare(kv, g, in, loc, &im->d, &im->n, &im->a, &im->b);
  if (rc) { free(im); *out = NULL; return rc; } *out = im; return 0; }
int efs_kv_import_step(struct efs_kv_import *im, int *done)
{ uint32_t n = im->n - im->at > 1024 ? 1024 : im->n - im->at; int rc = n ? efs_kv_batch(im->kv, im->d + im->at, n) : 0;
  if (rc) return rc; im->at += n; *done = im->at >= im->n; return 0; }
void efs_kv_import_stat(const struct efs_kv_import *im, uint32_t *a, uint32_t *b, uint64_t *c, uint64_t *d)
{ *a = N_IN; *b = LO_LAST - LO_FIRST + 1; *c = 0; *d = im->n; }
void efs_kv_import_close(struct efs_kv_import *im) { if (im) { free(im->d); free(im->a); free(im->b); free(im); } }


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

/* A batch that fails is applied again by the next step: no key is skipped. */
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

int main(void)
{
    test_bounded();
    test_interrupted();
    test_failed_batch();
    test_refused();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_kv_snap_import: ok\n");
    return 0;
}
