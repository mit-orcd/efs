/* AZ: one fixture matrix through every real verification wrapper. A chunk
 * fragment and a segment fragment are the same bytes (payload || 4 KiB tail
 * page holding the I25 record), so each case is built once per object format
 * and differs only in the identity the record names. Rules checked:
 *  - an object that does not match is never served, audited OK or kept as
 *    proven;
 *  - a read error is never absence, corruption or a reason to delete;
 *  - a missing file is absence.
 * The observed matrix is printed; justified differences are in
 * docs/status/az-verification-policy-audit-20261029.md. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *fail_open_path;
static int fail_open_errno;
static int hook_open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if (fail_open_errno && fail_open_path && !strcmp(path, fail_open_path)) {
        errno = fail_open_errno;
        return -1;
    }
    return open(path, flags, mode);
}
#define open hook_open
#include "../src/server/storage_roots.c"
#include "../src/server/store.c"
#include "../src/server/reclaim.c"
#include "../src/server/segment_store.c"
#include "test_tmpdir.h"
#undef open

enum { K_VALID, K_KIND, K_GEN, K_HOLDER, K_GEOM, K_SHORT, K_EXTRA, K_BODY, K_DIGEST,
       K_ABSENT, K_EIO, K_EACCES, K_COUNT };
static const char *const kname[K_COUNT] = {"valid", "wrong-kind", "wrong-generation",
    "wrong-holder", "wrong-geometry", "short-tail", "extra-tail", "bad-body",
    "wrong-digest", "absent", "read-EIO", "read-EACCES"};
enum { V_SERVED, V_ABSENT, V_REFUSED, V_IOERR, V_DROPPED, V_KEPT, V_NA };
static const char *const vname[] = {"served/proven", "absent", "refused/bad", "io-error",
                                    "dropped", "kept", "-"};
static char g_row[16][K_COUNT][24];

/* Write payload || tail with the record built from `id`; K_* shapes the file. */
static void make(const char *path, const uint8_t *pl, uint32_t len,
                 const struct efs_frag_ident *good, const struct efs_frag_ident *other, int kind)
{
    struct efs_frag_ident id = *good;
    uint8_t digest[EFS_HASH_SIZE], rec[EFS_FRAG_RECORD_BYTES];
    efs_hash(pl, len, digest);
    if (kind == K_KIND) id = *other; /* a valid record of the other object kind */
    if (kind == K_GEN) id.generation++;
    if (kind == K_HOLDER) id.fragment_index = (good->fragment_index + 1) % EFS_NUM_FRAGMENTS;
    if (kind == K_GEOM) id.payload_len = len / 2;
    assert(!efs_frag_record_encode(&id, digest, rec));
    uint8_t *buf = calloc(1, (size_t)len + 4096 * 2);
    memcpy(buf, pl, len);
    memcpy(buf + len, rec, sizeof(rec));
    if (kind == K_BODY) buf[10] ^= 0x55;
    size_t total = (size_t)len + 4096 + (kind == K_EXTRA ? 4096 : 0);
    if (kind == K_SHORT) total = (size_t)len + 64;
    make_dir_for_file(path);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    assert(fd >= 0 && write(fd, buf, total) == (ssize_t)total);
    close(fd);
    free(buf);
}

static int classify_rc(int rc, int absent_rc)
{
    if (rc == EFS_OK) return V_SERVED;
    if (rc == absent_rc) return V_ABSENT;
    if (rc == EFS_ERR_IO) return V_IOERR;
    return V_REFUSED;
}

static void inject(int kind, const char *path)
{
    fail_open_path = path;
    fail_open_errno = kind == K_EIO ? EIO : kind == K_EACCES ? EACCES : 0;
}

static void record(int w, int k, int v) { snprintf(g_row[w][k], sizeof(g_row[w][k]), "%s", vname[v]); }

/* The generic rule check; kind K_* against a verdict. */
static void rules(const char *w, int k, int v, int tolerates_extra)
{
    if (k == K_VALID && v != V_SERVED && v != V_KEPT) { fprintf(stderr, "%s: valid -> %s\n", w, vname[v]); assert(0); }
    if ((k == K_KIND || k == K_GEN || k == K_HOLDER || k == K_GEOM || k == K_SHORT || k == K_BODY) &&
        (v == V_SERVED || v == V_KEPT)) { fprintf(stderr, "%s: %s -> %s\n", w, kname[k], vname[v]); assert(0); }
    if (k == K_EXTRA && !tolerates_extra && (v == V_SERVED)) { fprintf(stderr, "%s: extra-tail served\n", w); assert(0); }
    if (k == K_ABSENT && v != V_ABSENT) { fprintf(stderr, "%s: absent -> %s\n", w, vname[v]); assert(0); }
    if ((k == K_EIO || k == K_EACCES) && v != V_IOERR) { fprintf(stderr, "%s: %s -> %s\n", w, kname[k], vname[v]); assert(0); }
}

int main(void)
{
    char root[1024], path[8192];
    efs_test_path(root, sizeof(root), "efs-az-XXXXXX");
    assert(efs_test_mkdtemp(root));
    struct efsd_server *s = calloc(1, sizeof(*s));
    assert(s && !pthread_mutex_init(&s->lock, NULL));
    s->storage_path_count = 1;
    strcpy(s->storage_paths[0], root);
    s->node_count = 1;
    s->nodes[0].id = s->id = 1;
    s->nodes[0].used = 1ull << 30;
    struct efs_export ex = {.id = 1};
    const efs_ino_t ino = 12345;
    const uint32_t ci = 7, fi = 1;
    const uint64_t gen = 0x77;
    const uint64_t sid = 0x1234, sgen = 3;
    const uint32_t len = server_frag_len(&ex, ino);
    uint32_t geometry = 2 * len;
    assert(geometry >= EFS_SEGMENT_ALIGNMENT && geometry % EFS_SEGMENT_ALIGNMENT == 0);
    uint8_t *pl = malloc(len), *out = malloc(len);
    for (uint32_t i = 0; i < len; i++) pl[i] = (uint8_t)(i * 31 + 7);
    uint8_t digest[EFS_HASH_SIZE], wrong[EFS_HASH_SIZE];
    efs_hash(pl, len, digest);
    memcpy(wrong, digest, sizeof(wrong));
    wrong[0] ^= 1;
    struct efs_frag_ident cid, sidn;
    efs_tls_chunk_gen = gen;
    fragment_ident(&ex, ino, ci, fi, len, &cid);
    sg_ident(0, sid, sgen, fi, len, &sidn);
    char cpath[8192], spath[8192];
    fragment_path_at(s, 0, &ex, ino, ci, fi, cpath, sizeof(cpath));
    efs_tls_chunk_gen = 0;
    server_segment_path(s, 0, sid, sgen, fi, spath, sizeof(spath));

    for (int k = 0; k < K_COUNT; k++) {
        struct server_recovery_files c;
        int rc, v;
        uint32_t got;
        uint8_t sum[EFS_HASH_SIZE];
        int sum_ok;
        const uint8_t *exp = k == K_DIGEST ? wrong : digest;
        (void)path;

#define PREP(P, ID, OTHER) do { unlink(P); fail_open_errno = 0; \
        if (k != K_ABSENT) make(P, pl, len, &ID, &OTHER, (k == K_EIO || k == K_EACCES || k == K_DIGEST) ? K_VALID : k); \
        inject(k, P); } while (0)

        /* 0 ordinary GET */
        PREP(cpath, cid, sidn);
        efs_tls_chunk_gen = gen;
        got = 0;
        rc = server_read_fragment_with_sum(s, &ex, ino, ci, fi, out, &got, sum, &sum_ok);
        efs_tls_chunk_gen = 0;
        v = classify_rc(rc, EFS_ERR_NOT_FOUND);
        if (k == K_DIGEST) v = V_SERVED; /* GET returns the record digest; the caller compares */
        record(0, k, v);
        rules("get", k, v, 1);

        /* 1 chunk recovery */
        PREP(cpath, cid, sidn);
        memset(&c, 0, sizeof(c));
        rc = server_recovery_fragment_check(s, 1, ino, ci, fi, gen, exp, &c);
        v = classify_rc(rc, EFS_ERR_NOT_FOUND);
        if (c.dropped) v = V_DROPPED;
        record(1, k, v);
        if (k == K_VALID) assert(v == V_SERVED);
        if (k == K_ABSENT) assert(v == V_ABSENT);
        if (k == K_EIO || k == K_EACCES) { assert(v == V_IOERR && !c.dropped); fail_open_errno = 0; assert(!access(cpath, F_OK)); }
        if (k >= K_KIND && k <= K_BODY && k != K_EXTRA) assert(v == V_DROPPED);
        if (k == K_DIGEST) assert(v == V_DROPPED);

        /* 2 chunk GC delete (acts on a record that proves this object) */
        PREP(cpath, cid, sidn);
        efs_tls_chunk_gen = gen;
        rc = server_delete_fragment_if_sum(s, &ex, ino, ci, fi, digest);
        efs_tls_chunk_gen = 0;
        fail_open_errno = 0;
        v = access(cpath, F_OK) ? V_DROPPED : V_KEPT;
        if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND && k != K_VALID) { /* refused */ }
        if (k == K_ABSENT) v = V_NA;
        record(2, k, v);
        if (k == K_EIO || k == K_EACCES) assert(v == V_KEPT);
        if (k == K_KIND || k == K_GEN || k == K_HOLDER || k == K_GEOM) assert(v == V_KEPT);

        /* 3 segment GET (server_segment_read) */
        PREP(spath, sidn, cid);
        rc = server_segment_read(s, 0, sid, sgen, fi, len, out, sum);
        v = classify_rc(rc, EFS_ERR_NOT_FOUND);
        if (k == K_DIGEST) v = V_SERVED;
        record(3, k, v);
        rules("segment-read", k, v, 1);

        /* 4 segment audit */
        PREP(spath, sidn, cid);
        uint32_t a = server_segment_check(s, 0, sid, sgen, fi, geometry, exp);
        v = a == EFS_SEGMENT_AUDIT_OK ? V_SERVED : a == EFS_SEGMENT_AUDIT_MISSING ? V_ABSENT
          : a == EFS_SEGMENT_AUDIT_ERROR ? V_IOERR : V_REFUSED;
        record(4, k, v);
        if (k == K_DIGEST) assert(v == V_REFUSED);
        else rules("segment-audit", k, v, 0);

        /* 5 segment recovery */
        PREP(spath, sidn, cid);
        memset(&c, 0, sizeof(c));
        rc = server_segment_recovery_check(s, 0, sid, sgen, fi, exp, &c);
        v = classify_rc(rc, EFS_ERR_NOT_FOUND);
        if (c.dropped) v = V_DROPPED;
        record(5, k, v);
        if (k == K_VALID) assert(v == V_SERVED);
        if (k == K_ABSENT) assert(v == V_ABSENT);
        if (k == K_EIO || k == K_EACCES) { assert(v == V_IOERR && !c.dropped); fail_open_errno = 0; assert(!access(spath, F_OK)); }
        if (k >= K_KIND && k <= K_BODY && k != K_EXTRA) assert(v == V_DROPPED);
        if (k == K_DIGEST) assert(v == V_DROPPED);
    }
    static const char *const wname[] = {"GET (chunk)", "recovery (chunk)", "GC delete (chunk)",
        "segment read", "segment audit", "recovery (segment)"};
    printf("%-20s", "wrapper");
    for (int k = 0; k < K_COUNT; k++) printf(" | %s", kname[k]);
    printf("\n");
    for (int w = 0; w < 6; w++) {
        printf("%-20s", wname[w]);
        for (int k = 0; k < K_COUNT; k++) printf(" | %s", g_row[w][k]);
        printf("\n");
    }
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    assert(!system(cmd));
    free(pl); free(out); free(s);
    puts("verification matrix: PASS");
    return 0;
}
