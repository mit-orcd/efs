#!/usr/bin/env python3
"""D42 READ_VERIFY audit: every client GET path refuses wrong bytes.

Builds the production fragment readers of src/client/read.c (the chunk
fetch, the paired data GET, parity reconstruction, retries, span overlays
and packed segment reads) against fake storage nodes that answer over real
efs framing on socketpairs (the TCP receive code, efs_conn_recv_msg_into).
Each node can corrupt or substitute its reply at a chosen point: a payload
byte flipped after the node hashed it (the wire or the client buffer), the
right key answered with another fragment and that fragment's digest, bytes
and digest both rewritten (a self-consistent wrong reply), an INTEGRITY
refusal (the node's own record check; tests/test_store_gc.c drives that on
real files), absence and a dropped connection. Every case reads with no
EFS_* setting: a read either returns the exact written bytes, decoded from
verified fragments only, or fails with EFS_ERR_CHECKSUM.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
read = (root / 'src/client/read.c').read_text()


def function(name):
    match = re.search(r'^(?:static )?[^\n;]*\b' + name + r'\([^;]*?\)\n\{', read, re.M)
    assert match, name
    return match.start(), read[match.start():read.index('\n}\n', match.end()) + 3]


def block(start, end):
    a = read.index(start)
    return a, read[a:read.index(end, a) + len(end)]


# The production checks this test depends on: no switch, a hash per reply.
assert 'EFS_READ_VERIFY' not in read and 'read_verify' not in read
names = ('data_chunk_size', 'data_frag_size', 'frag_ptrs', 'frag_get_thread',
         'read_frag_prof_init', 'fragment_reply_verified', 'get_one_reply',
         'get_pair_start', 'get_two_parallel_run', 'get_two_parallel',
         'read_peer_now_ms', 'packed_fetch_run', 'packed_fetch', 'packed_short', 'read_reprobe_down',
         'efs_client_decode_placed_chunk_attempts', 'fetch_published_once',
         'get_fragment_run', 'efs_client_get_fragment', 'overlay_one_delta_ex', 'overlay_one_delta',
         'overlay_chunk_deltas_ce')
parts = [function(n) for n in names]
parts.append(block('struct frag_get_job {', '\n};\n'))
parts.append(block('struct packed_fetch_ctx {', '};\n'))
production = '\n'.join(text for _, text in sorted(parts))

# The read cache is filled only from a fetch that returned EFS_OK.
worker = function('chunk_get_worker')[1]
fill = worker.index('if (job->rc == EFS_OK) {')
assert worker.count('rdcache_put_inner(') == 2
assert all(m.start() > fill for m in re.finditer(r'rdcache_put_inner\(', worker))
assert worker.rindex('rdcache_put_inner(') < worker.index('} else if (acq == 1)', fill)

source = r'''
#define _GNU_SOURCE
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/metadata.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include "efs/packed_reader.h"
#include "read_peer_order.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#define FL (EFS_DEFAULT_CHUNK_SIZE / 2)
#define CS EFS_DEFAULT_CHUNK_SIZE
#define BASE_GEN 77u
#define SPAN_GEN 88u
#define INO 4242u
/* The span covers the end of D1 and the start of D2, and the flipped bytes
 * land inside it in either data fragment (and in the packed slice). */
#define SPAN_OFF (FL - 4096u)
#define SPAN_LEN 16384u
#define FLIP_LO 200u
#define FLIP_HI (FL - 1000u)

static struct {
    struct { uint32_t chunk_size; } export;
    pthread_mutex_t idx_mu;
    efs_node_id_t local_node_id;
    efs_export_id_t export_id;
} g_client = { { CS }, PTHREAD_MUTEX_INITIALIZER, 0, 1 };
static struct efs_chunk_entry row;
static int test_export_get_chunk(void *ex, efs_ino_t ino, uint32_t ci,
                                 struct efs_chunk_entry *out)
{
    (void)ex;
    if (ino != INO || ci != 0) return EFS_ERR_NOT_FOUND;
    if (out) *out = row;
    return EFS_OK;
}
#define efs_export_get_chunk(ex, ino, ci, out) test_export_get_chunk(ex, ino, ci, out)
static __thread uint64_t deadline_ms;
static uint64_t efs_client_rpc_deadline_ms(void) { return deadline_ms; }
static void efs_client_rpc_set_deadline_ms(uint64_t d) { deadline_ms = d; }
static int efs_client_rpc_past_deadline(void) { return 0; }
static unsigned down_mask, reprobe_mask;
static int efs_client_node_is_down(efs_node_id_t n) { return !!(down_mask & (1u << n)); }
static int efs_client_node_reprobe(efs_node_id_t n)
{
    if (!(reprobe_mask & (1u << n))) return 0;
    down_mask &= ~(1u << n);
    return 1;
}
static void efs_client_node_note_ok(efs_node_id_t n) { (void)n; }
static void efs_client_node_note_fail(efs_node_id_t n) { (void)n; }
static int read_retry_pause(unsigned us) { (void)us; return EFS_OK; }
/* Production down-member recovery, with deterministic probe outcomes. */
static void *demand_read_alloc(uint32_t n) { return malloc(n); }
static void test_buf_free(void *p, uint32_t n) { (void)n; free(p); }
#define efs_buf_free test_buf_free
static uint8_t scratch_buf[EFS_NUM_FRAGMENTS * FL];
static uint8_t *decode_frag_scratch(uint32_t need)
{ return need <= sizeof(scratch_buf) ? scratch_buf : NULL; }
static pthread_once_t read_frag_prof_once = PTHREAD_ONCE_INIT;
static int read_frag_prof;
static const int read_frag_timeout_ms = 2000;
static struct efs_read_peer_order read_peer_order;

/* ---- fake storage nodes -------------------------------------------- */
enum { M_OK, M_FLIP, M_SUBST, M_LIE, M_INTEGRITY, M_NOTFOUND, M_CLOSE };
enum { K_BASE, K_SPAN, K_SEG, K_KINDS };
static uint8_t frag[K_KINDS][3][FL], sums[K_KINDS][3][EFS_HASH_SIZE];
static uint32_t seg_fl;
/* mode[kind][fi][n]: the reply to the n-th request (the last repeats). */
static int mode[K_KINDS][3][4];
static int served[K_KINDS][3];
static pthread_mutex_t serve_mu = PTHREAD_MUTEX_INITIALIZER;

static void reset(void)
{
    pthread_mutex_lock(&serve_mu);
    memset(mode, 0, sizeof(mode));
    memset(served, 0, sizeof(served));
    pthread_mutex_unlock(&serve_mu);
    memset(&read_peer_order, 0, sizeof(read_peer_order));
    down_mask = reprobe_mask = 0;
}
static void set_mode(int kind, int fi, int m0, int m1)
{ mode[kind][fi][0] = m0; mode[kind][fi][1] = m1; mode[kind][fi][2] = m1; mode[kind][fi][3] = m1; }
static int take(int kind, int fi)
{
    pthread_mutex_lock(&serve_mu);
    int n = served[kind][fi]++;
    int m = mode[kind][fi][n < 3 ? n : 3];
    pthread_mutex_unlock(&serve_mu);
    return m;
}
/* Payload and digest the node sends for this mode. 0: send them. */
static int reply_body(int kind, int fi, int m, uint32_t len, uint8_t *body,
                      uint8_t sum[EFS_HASH_SIZE])
{
    int src = m == M_SUBST ? (fi + 1) % 3 : fi;
    memcpy(body, frag[kind][src], len);
    memcpy(sum, sums[kind][src], EFS_HASH_SIZE);
    if (m == M_FLIP || m == M_LIE) {    /* after the node's own hash */
        body[FLIP_LO % len] ^= 0x20;    /* inside the span in either half */
        body[FLIP_HI % len] ^= 0x20;
    }
    if (m == M_LIE)
        efs_hash(body, len, sum);       /* a self-consistent wrong reply */
    return m == M_OK || m == M_FLIP || m == M_SUBST || m == M_LIE ? 0 : m;
}
static void *serve(void *arg)
{
    int fd = (int)(intptr_t)arg;
    static __thread uint8_t reply[1 + EFS_HASH_SIZE + FL + EFS_SEGMENT_GET_REPLY];
    for (;;) {
        uint8_t type = 0; void *req = NULL; uint32_t len = 0;
        if (efs_recv_msg(fd, &type, &req, &len) != EFS_OK) break;
        if (type == EFS_MSG_GET_CHUNK && len == sizeof(struct efs_msg_get_chunk)) {
            struct efs_msg_get_chunk *g = req;
            int kind = g->chunk_generation == BASE_GEN ? K_BASE :
                       g->chunk_generation == SPAN_GEN ? K_SPAN : -1;
            assert(kind >= 0 && g->ino == INO && g->chunk_index == 0 && g->fragment_index < 3);
            int fi = (int)g->fragment_index, m = take(kind, fi);
            int r = reply_body(kind, fi, m, FL, reply + 1 + EFS_HASH_SIZE, reply + 1);
            free(req);
            if (r == M_CLOSE) break;
            reply[0] = r == 0 ? EFS_GET_CHUNK_OK :
                       r == M_INTEGRITY ? EFS_GET_CHUNK_INTEGRITY : EFS_GET_CHUNK_NOT_FOUND;
            if (efs_send_msg(fd, EFS_MSG_GET_CHUNK_REPLY, reply,
                             r == 0 ? 1 + EFS_HASH_SIZE + FL : 1)) break;
        } else if (type == EFS_MSG_GET_SEGMENT && len == EFS_SEGMENT_GET_HEADER) {
            const uint8_t *q = req;
            int fi = q[1], m = take(K_SEG, fi);
            free(req);
            memset(reply, 0, EFS_SEGMENT_GET_REPLY);
            int r = reply_body(K_SEG, fi, m, seg_fl, reply + EFS_SEGMENT_GET_REPLY, reply + 8);
            if (r == M_CLOSE) break;
            uint32_t status = r == 0 ? 0 : (uint32_t)(r == M_INTEGRITY ? EFS_ERR_CHECKSUM : EFS_ERR_NOT_FOUND);
            uint32_t bytes = r == 0 ? seg_fl : 0;
            for (int i = 0; i < 4; i++) {
                reply[i] = (uint8_t)(status >> (24 - 8 * i));
                reply[4 + i] = (uint8_t)(bytes >> (24 - 8 * i));
            }
            if (efs_send_msg(fd, EFS_MSG_GET_SEGMENT_REPLY, reply, EFS_SEGMENT_GET_REPLY + bytes)) break;
        } else {
            free(req);
            break;
        }
    }
    close(fd);
    return NULL;
}
/* One fresh connection per checkout; release and drop both close it. */
static struct efs_conn *efs_client_conn_get(efs_node_id_t node)
{
    int sv[2];
    pthread_t t;
    assert(node >= 1 && node <= 3);
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv));
    assert(!pthread_create(&t, NULL, serve, (void *)(intptr_t)sv[1]));
    pthread_detach(t);
    return efs_conn_wrap_tcp(sv[0], 0);
}
static void efs_client_conn_release(efs_node_id_t n, struct efs_conn *c) { (void)n; efs_conn_destroy(c); }
static void efs_client_conn_drop(efs_node_id_t n, struct efs_conn *c) { (void)n; efs_conn_destroy(c); }

struct frag_get_job;
static void get_two_parallel(struct frag_get_job *jobs);
static int overlay_chunk_deltas_ce(efs_ino_t ino, uint32_t ci, uint8_t *buf,
                                   uint32_t chunk_len, const struct efs_chunk_entry *cep);
int efs_client_get_fragment(efs_node_id_t node_id, efs_ino_t ino,
                            uint32_t chunk_index, uint32_t fragment_index,
                            uint32_t expected_frag_len, uint8_t *data,
                            uint32_t *data_len, uint8_t checksum[EFS_HASH_SIZE],
                            uint64_t chunk_generation);
''' + production + r'''
/* ---- cases ----------------------------------------------------------- */
static uint8_t base_image[CS], span_image[CS], file_image[CS], seg_payload[512];
static uint8_t out[CS];
static const uint32_t span_off = SPAN_OFF, span_len = SPAN_LEN;
static struct efs_pack_ref seg_ref, seg_refs[4];

static void fill(uint8_t *p, size_t n, uint32_t seed)
{ for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; p[i] = (uint8_t)(seed >> 16); } }

static void make_row(int packed, int with_span)
{
    memset(&row, 0, sizeof(row));
    row.ino = INO;
    row.generation = BASE_GEN;
    for (int i = 0; i < 3; i++) {
        row.fragment_nodes[i] = (efs_node_id_t)(i + 1);
        memcpy(row.checksums[i], sums[packed ? K_SEG : K_BASE][i], EFS_HASH_SIZE);
    }
    if (packed) {
        row.packed = seg_ref;
        row.packed.ino = INO;
        row.packed.record_generation = BASE_GEN;
    }
    if (with_span) {
        row.ndelta = 1;
        row.delta_seq = 1;
        row.deltas[0].off = span_off;
        row.deltas[0].len = span_len;
        row.deltas[0].generation = SPAN_GEN;
        row.deltas[0].seq = 1;
        for (int i = 0; i < 3; i++) {
            row.deltas[0].nodes[i] = (efs_node_id_t)(i + 1);
            memcpy(row.deltas[0].checksums[i], sums[K_SPAN][i], EFS_HASH_SIZE);
        }
    }
}
static int decode(int attempts)
{
    memset(out, 0xee, sizeof(out));
    return efs_client_decode_placed_chunk_attempts(INO, 0, out, CS, FL, attempts, 0, &row);
}
#define GOOD(rc, image) do { int r_ = (rc); assert(r_ == EFS_OK); \
    assert(!memcmp(out, (image), CS)); } while (0)
/* image: the true bytes, for a run that reports what a failure returned. */
#define REFUSED(rc, image) do { int r_ = (rc); (void)(image); if (r_ != EFS_ERR_CHECKSUM) { \
    fprintf(stderr, "line %d: rc=%d\n", __LINE__, r_); } assert(r_ == EFS_ERR_CHECKSUM); } while (0)

static void chunk_get_cases(void)
{
    static const int bad[] = { M_FLIP, M_SUBST, M_LIE };
    make_row(0, 0);
    reset(); GOOD(decode(1), base_image);
    assert(served[K_BASE][0] == 1 && served[K_BASE][1] == 1 && !served[K_BASE][2]);
    for (unsigned b = 0; b < 3; b++) {
        /* Ordinary chunk GET: one data fragment wrong on receive. It is
         * refused and the chunk is decoded from D2 + parity. */
        for (int fi = 0; fi < 2; fi++) {
            reset(); set_mode(K_BASE, fi, bad[b], bad[b]);
            GOOD(decode(1), base_image);
            assert(served[K_BASE][2] == 1);
        }
        /* Both data fragments wrong: parity alone cannot decode. */
        reset(); set_mode(K_BASE, 0, bad[b], bad[b]); set_mode(K_BASE, 1, bad[b], bad[b]);
        REFUSED(decode(1), base_image);
        /* Reconstruction: D1 missing, parity wrong. */
        reset(); set_mode(K_BASE, 0, M_NOTFOUND, M_NOTFOUND); set_mode(K_BASE, 2, bad[b], bad[b]);
        REFUSED(decode(1), base_image);
        /* Reconstruction: D2 refused by its node, parity wrong. */
        reset(); set_mode(K_BASE, 1, M_INTEGRITY, M_INTEGRITY); set_mode(K_BASE, 2, bad[b], bad[b]);
        REFUSED(decode(1), base_image);
    }
    /* Reconstruction from verified fragments still works. */
    reset(); set_mode(K_BASE, 0, M_NOTFOUND, M_NOTFOUND);
    GOOD(decode(1), base_image);
    reset(); set_mode(K_BASE, 1, M_INTEGRITY, M_INTEGRITY);
    GOOD(decode(1), base_image);
    /* Two node refusals (on-disk rot or another identity at the node). */
    reset(); set_mode(K_BASE, 0, M_INTEGRITY, M_INTEGRITY); set_mode(K_BASE, 2, M_INTEGRITY, M_INTEGRITY);
    REFUSED(decode(1), base_image);
    /* Retries: the parity GET's first connection drops, the retry's reply
     * is corrupt. A retry never launders it. */
    reset(); set_mode(K_BASE, 0, M_NOTFOUND, M_NOTFOUND); set_mode(K_BASE, 2, M_CLOSE, M_FLIP);
    REFUSED(decode(1), base_image);
    assert(served[K_BASE][2] == 2);
    /* A second attempt of the whole read: corrupt replies first, good
     * replies on the next attempt return the exact bytes. */
    reset(); set_mode(K_BASE, 0, M_FLIP, M_OK); set_mode(K_BASE, 1, M_LIE, M_OK); set_mode(K_BASE, 2, M_FLIP, M_OK);
    GOOD(decode(2), base_image);
    reset(); set_mode(K_BASE, 0, M_FLIP, M_FLIP); set_mode(K_BASE, 1, M_SUBST, M_SUBST);
    REFUSED(decode(3), base_image);
    puts("chunk GET, parity reconstruction, node refusals and retries: PASS");
}

static void span_cases(void)
{
    static const int bad[] = { M_FLIP, M_SUBST, M_LIE };
    make_row(0, 1);
    reset();
    memset(out, 0xee, sizeof(out));
    GOOD(fetch_published_once(INO, 0, out, CS, NULL, NULL), file_image);
    for (unsigned b = 0; b < 3; b++) {
        /* One span fragment wrong: decoded from the other two. */
        reset(); set_mode(K_SPAN, 1, bad[b], bad[b]);
        memset(out, 0xee, sizeof(out));
        GOOD(fetch_published_once(INO, 0, out, CS, NULL, NULL), file_image);
        /* Two wrong: the overlay, so the read, fails; the base image the
         * span would paint is left as it was. */
        reset(); set_mode(K_SPAN, 0, bad[b], bad[b]); set_mode(K_SPAN, 2, bad[b], bad[b]);
        memcpy(out, base_image, CS);
        REFUSED(overlay_one_delta(INO, 0, out, CS, &row.deltas[0]), file_image);
        assert(!memcmp(out, base_image, CS));
        memset(out, 0xee, sizeof(out));
        REFUSED(fetch_published_once(INO, 0, out, CS, NULL, NULL), file_image);
    }
    /* The base wrong under a good span: the whole fetch fails. */
    reset(); set_mode(K_BASE, 0, M_LIE, M_LIE); set_mode(K_BASE, 1, M_FLIP, M_FLIP);
    memset(out, 0xee, sizeof(out));
    REFUSED(fetch_published_once(INO, 0, out, CS, NULL, NULL), file_image);
    puts("span overlays and the published-chunk fetch (read-cache fill): PASS");
}

static void packed_cases(void)
{
    static const int bad[] = { M_FLIP, M_SUBST, M_LIE };
    uint8_t expect[CS];
    memset(expect, 0, sizeof(expect));
    memcpy(expect, seg_payload, sizeof(seg_payload));
    make_row(1, 0);
    reset(); GOOD(decode(1), expect);
    for (unsigned b = 0; b < 3; b++) {
        for (int fi = 0; fi < 3; fi++) {
            reset(); set_mode(K_SEG, fi, bad[b], bad[b]);
            GOOD(decode(1), expect);
        }
        reset(); set_mode(K_SEG, 0, bad[b], bad[b]); set_mode(K_SEG, 1, bad[b], bad[b]);
        REFUSED(decode(1), expect);
        reset(); set_mode(K_SEG, 1, M_NOTFOUND, M_NOTFOUND); set_mode(K_SEG, 2, bad[b], bad[b]);
        REFUSED(decode(1), expect);
    }
    /* A returned holder plus one live holder is enough even when the last
     * fragment is absent. The reader's last-error order must not suppress
     * the bounded down-member re-probe (K36 pool failure, step 8). */
    for (int returned = 0; returned < 3; returned++) {
        for (int missing = 0; missing < 3; missing++) {
            if (returned == missing) continue;
            reset();
            down_mask = reprobe_mask = 1u << (returned + 1);
            set_mode(K_SEG, missing, M_NOTFOUND, M_NOTFOUND);
            GOOD(decode(1), expect);
            assert(!down_mask);
            /* A probe that cannot reach the holder still refuses; no
             * absent fragment or unchecked payload becomes a success. */
            reset(); down_mask = 1u << (returned + 1);
            set_mode(K_SEG, missing, M_NOTFOUND, M_NOTFOUND);
            assert(decode(1) == EFS_ERR_IO);
        }
    }
    /* A verified segment, but the reference names a neighbour's slice
     * (offset inside the segment) or carries another digest: refused by
     * the record check, never served as this file's bytes. */
    struct efs_pack_ref keep = row.packed;
    row.packed.offset = seg_refs[1].offset;
    reset(); memset(out, 0xee, sizeof(out));
    assert(decode(1) == EFS_ERR_STALE);
    row.packed = keep; row.packed.digest[3] ^= 1;
    reset(); assert(decode(1) == EFS_ERR_STALE);
    row.packed = keep; row.packed.len += 1;
    reset(); assert(decode(1) != EFS_OK);
    puts("packed segment reads and slices inside a segment: PASS");
}

int main(void)
{
    uint8_t *f[3];
    fill(base_image, CS, 1);
    for (int i = 0; i < 3; i++) f[i] = frag[K_BASE][i];
    assert(!efs_encode_chunk(base_image, CS, CS, f));
    fill(span_image, CS, 2);
    for (int i = 0; i < 3; i++) f[i] = frag[K_SPAN][i];
    assert(!efs_encode_chunk(span_image, CS, CS, f));
    memcpy(file_image, base_image, CS);
    memcpy(file_image + span_off, span_image + span_off, span_len);

    /* A segment of four packed files; the file under test is record 0.
     * The neighbours fill both halves, so no two fragments are equal. */
    static uint8_t image[EFS_SEGMENT_MAX_SIZE], others[3][4000];
    struct efs_pack_record rec[4];
    fill(seg_payload, sizeof(seg_payload), 3);
    rec[0] = (struct efs_pack_record){INO, 7, 0, BASE_GEN, sizeof(seg_payload), seg_payload};
    for (int i = 1; i < 4; i++) {
        fill(others[i - 1], sizeof(others[0]), 10 + (uint32_t)i);
        rec[i] = (struct efs_pack_record){INO + (efs_ino_t)i, 7, 0, BASE_GEN + (uint64_t)i,
                                          sizeof(others[0]), others[i - 1]};
    }
    size_t geometry;
    assert(!efs_pack_build(51, 53, 0, rec, 4, image, sizeof(image), &geometry, seg_refs));
    seg_fl = (uint32_t)geometry / 2;
    assert(seg_fl <= FL);
    for (int i = 0; i < 3; i++) f[i] = frag[K_SEG][i];
    assert(!efs_encode_segment(image, geometry, geometry, f));
    seg_ref = seg_refs[0];
    for (int k = 0; k < K_KINDS; k++)
        for (int i = 0; i < 3; i++)
            efs_hash(frag[k][i], k == K_SEG ? seg_fl : FL, sums[k][i]);
    /* A substituted fragment must differ from the requested one. */
    for (int k = 0; k < K_KINDS; k++)
        for (int i = 0; i < 3; i++)
            assert(memcmp(sums[k][i], sums[k][(i + 1) % 3], EFS_HASH_SIZE));

    chunk_get_cases();
    span_cases();
    packed_cases();
    puts("GET integrity with no EFS_* setting: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='efs-get-integrity-', dir=os.environ.get('TMPDIR')) as directory:
    path = Path(directory) / 'test.c'
    path.write_text(source)
    binary = path.with_suffix('')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-I' + str(root / 'include'), '-I' + str(root / 'src/client'),
                    str(path), str(root / 'libefs.a'), '-lm', '-ldl', '-libverbs', '-o', str(binary)],
                   check=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_')}
    # Refusals log one line each (read.c); show them only on a failure.
    run = subprocess.run([str(binary)], env=env, timeout=60, capture_output=True, text=True)
    print(run.stdout, end='')
    if run.returncode:
        print(run.stderr, end='')
        raise SystemExit('test_get_integrity: exit %d' % run.returncode)
