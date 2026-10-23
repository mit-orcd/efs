#!/usr/bin/env python3
"""W77 client publication units: the production unit tracker and REPORT
shaping (src/client/write.c) compiled against mocked put/dcache state.

A multi-chunk write is one unit, merged with every unit it shares a chunk
with; a merge past EFS_PUB_UNIT_MAX asks for a drain. A REPORT carries a
unit only whole (adjacent records joined by UNIT_NEXT) when no write holds
it and every chunk's record names its newest landed object; otherwise the
whole unit is held back for re-marking. Only the carried, unchanged unit
retires after success."""
from pathlib import Path
import subprocess, tempfile

root = Path(__file__).resolve().parents[1]
s = (root / 'src/client/write.c').read_text()
a = s.index('/* ---- W77 · atomic publication units (client_internal.h)')
b = s.index('/* The host committed OUR object `gen` for (ino,ci)', a)
block = s[a:b]
assert 'efs_client_unit_begin' in block and 'report_units_shape' in block

code = r'''
#include "efs/protocol.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CS 131072u
static uint32_t data_chunk_size(void) { return CS; }
static int deadline_passed;
int efs_client_rpc_past_deadline(void) { return deadline_passed; }
/* Mocked chunk state: newest landed object, settledness. */
struct put_id { efs_ino_t ino; uint32_t ci; uint64_t gen; };
struct dcache_ent { int dirty, stalled; uint64_t snap_seq, object_seq, object_gen; int unpub; };
static pthread_mutex_t g_putid_mu = PTHREAD_MUTEX_INITIALIZER, g_dmu = PTHREAD_MUTEX_INITIALIZER;
static struct put_id g_put[128];
static struct dcache_ent g_ent[128];
static int g_has_ent[128];
static struct put_id *putid_find(efs_ino_t ino, uint32_t ci)
{ return ino == 7 && ci < 128 && g_put[ci].gen ? &g_put[ci] : NULL; }
static void dcache_ensure(void) {}
static uint32_t dcache_slot(efs_ino_t ino, uint32_t ci) { (void)ino; return ci; }
static pthread_mutex_t *dcache_mu(uint32_t s) { (void)s; return &g_dmu; }
static struct dcache_ent *dcache_find_meta(uint32_t s, efs_ino_t ino, uint32_t ci)
{ (void)s; return ino == 7 && ci < 128 && g_has_ent[ci] ? &g_ent[ci] : NULL; }
static int dcache_unpublished(const struct dcache_ent *e) { return e->unpub; }
static unsigned marks;
void efs_client_mark_chunk_dirty(efs_ino_t ino, uint32_t ci) { (void)ino; (void)ci; marks++; }
void efs_client_mark_ino_dirty(efs_ino_t ino) { (void)ino; }
int efs_client_unit_begin(efs_ino_t ino, uint64_t offset, uint64_t len);
void efs_client_unit_end(efs_ino_t ino, uint64_t offset, uint64_t len);
''' + block + r'''
static struct efs_chunk_rec rec(efs_ino_t ino, uint32_t ci, uint64_t gen)
{ struct efs_chunk_rec r; memset(&r, 0, sizeof(r)); r.ino = ino; r.chunk_index = ci; r.chunk_generation = gen; return r; }
static void landed(uint32_t ci, uint64_t gen) { g_put[ci].ino = 7; g_put[ci].ci = ci; g_put[ci].gen = gen; }
static uint32_t shape(struct efs_chunk_rec *r, uint64_t *seq, uint32_t n,
                      struct unit_sent **sent, uint32_t *ns, struct report_held **held, uint32_t *nh)
{ assert(report_units_shape(r, seq, &n, sent, ns, held, nh) == EFS_OK); return n; }
int main(void)
{
    struct efs_chunk_rec r[8]; uint64_t seq[8] = {0};
    struct unit_sent *sent = NULL; struct report_held *held = NULL; uint32_t ns = 0, nh = 0, n;

    /* A one-chunk write is no unit; a two-chunk write is one. */
    assert(efs_client_unit_begin(7, 10, 100) == EFS_OK && !unit_find(7, 0));
    assert(efs_client_unit_begin(7, CS - 10, 20) == EFS_OK);
    struct unit_ino *u = unit_find(7, 0);
    assert(u && u->n == 1 && u->span[0].lo == 0 && u->span[0].hi == 1 && u->span[0].writers == 1);
    /* A write sharing chunk 1 joins it; one sharing nothing is separate. */
    assert(efs_client_unit_begin(7, CS + 5, CS) == EFS_OK);
    assert(u->n == 1 && u->span[0].lo == 0 && u->span[0].hi == 2 && u->span[0].writers == 2);
    assert(efs_client_unit_begin(7, 10 * (uint64_t)CS, 2 * CS) == EFS_OK && u->n == 2);
    /* Past the cap: publish first. */
    assert(efs_client_unit_begin(7, 2 * (uint64_t)CS, 70 * (uint64_t)CS) == EFS_OK && u->n == 2);
    assert(efs_client_unit_begin(7, 2 * (uint64_t)CS + 1, 63 * (uint64_t)CS) == EFS_ERR_AGAIN);

    /* Held: chunks 0..2 still have a writer. */
    landed(0, 0xA); landed(1, 0xB); landed(2, 0xC);
    r[0] = rec(7, 5, 0x5); r[1] = rec(7, 2, 0xC); r[2] = rec(7, 0, 0xA); r[3] = rec(7, 1, 0xB);
    n = shape(r, seq, 4, &sent, &ns, &held, &nh);
    assert(n == 1 && r[0].chunk_index == 5 && ns == 0 && nh == 3);
    report_units_remark(held, nh); assert(marks == 3); free(held); held = NULL; nh = 0;
    deadline_passed = 1; assert(unit_wait_idle(7) == EFS_ERR_BUSY); deadline_passed = 0;
    efs_client_unit_end(7, CS - 10, 20);
    efs_client_unit_end(7, CS + 5, CS);
    assert(u->span[0].writers == 0);

    /* Held: chunk 1 has a PUT in flight (the record names an older object). */
    g_has_ent[1] = 1; g_ent[1].snap_seq = 2; g_ent[1].object_seq = 1;
    r[0] = rec(7, 5, 0x5); r[1] = rec(7, 2, 0xC); r[2] = rec(7, 0, 0xA); r[3] = rec(7, 1, 0xB);
    n = shape(r, seq, 4, &sent, &ns, &held, &nh);
    assert(n == 1 && nh == 3); free(held); held = NULL; nh = 0;
    g_ent[1].object_seq = 2;
    /* Held: a newer object of chunk 1 landed after the record was built. */
    landed(1, 0xBB);
    r[0] = rec(7, 5, 0x5); r[1] = rec(7, 2, 0xC); r[2] = rec(7, 0, 0xA); r[3] = rec(7, 1, 0xB);
    n = shape(r, seq, 4, &sent, &ns, &held, &nh);
    assert(n == 1 && nh == 3); free(held); held = NULL; nh = 0;
    /* Held: chunk 1 landed but this REPORT has no record of it. */
    r[0] = rec(7, 5, 0x5); r[1] = rec(7, 2, 0xC); r[2] = rec(7, 0, 0xA);
    n = shape(r, seq, 3, &sent, &ns, &held, &nh);
    assert(n == 1 && nh == 2); free(held); held = NULL; nh = 0;

    /* Whole: adjacent, ordered, joined; records outside units first. */
    r[0] = rec(7, 5, 0x5); r[1] = rec(7, 2, 0xC); r[2] = rec(7, 0, 0xA); r[3] = rec(7, 1, 0xBB);
    seq[1] = 22; seq[2] = 20; seq[3] = 21;
    n = shape(r, seq, 4, &sent, &ns, &held, &nh);
    assert(n == 4 && nh == 0 && ns == 1 && sent[0].lo == 0 && sent[0].hi == 2);
    assert(r[0].chunk_index == 5 && !(r[0].publish_flags & EFS_CHUNK_REC_F_UNIT_NEXT));
    assert(r[1].chunk_index == 0 && seq[1] == 20 && (r[1].publish_flags & EFS_CHUNK_REC_F_UNIT_NEXT));
    assert(r[2].chunk_index == 1 && seq[2] == 21 && (r[2].publish_flags & EFS_CHUNK_REC_F_UNIT_NEXT));
    assert(r[3].chunk_index == 2 && seq[3] == 22 && !(r[3].publish_flags & EFS_CHUNK_REC_F_UNIT_NEXT));
    /* A write merging after the shape keeps the unit after success. */
    struct unit_sent kept = sent[0];
    assert(efs_client_unit_begin(7, 2 * (uint64_t)CS + 1, CS) == EFS_OK);
    report_units_published(&kept, 1);
    assert(u->n == 2);
    efs_client_unit_end(7, 2 * (uint64_t)CS + 1, CS);
    /* The carried, unchanged unit retires. */
    landed(3, 0xD);
    r[0] = rec(7, 0, 0xA); r[1] = rec(7, 1, 0xBB); r[2] = rec(7, 2, 0xC); r[3] = rec(7, 3, 0xD);
    n = shape(r, seq, 4, &sent, &ns, &held, &nh);
    assert(n == 4 && ns == 1 && sent[0].hi == 3);
    report_units_published(sent, ns);
    assert(u->n == 1 && u->span[0].lo == 10);
    /* An idle unit with nothing left to publish retires at the next shape. */
    efs_client_unit_end(7, 10 * (uint64_t)CS, 2 * CS);
    memset(g_put, 0, sizeof(g_put));
    r[0] = rec(7, 5, 0x5);
    n = shape(r, seq, 1, &sent, &ns, &held, &nh);
    assert(n == 1 && u->n == 0);
    free(sent);
    puts("publication units: merge, cap, hold, whole-unit framing and retirement PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pub-units-') as d:
    p = Path(d) / 't.c'
    p.write_text(code)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-pthread', '-I' + str(root / 'include'), str(p), '-o', str(p.with_suffix(''))],
                   check=True)
    out = subprocess.run([str(p.with_suffix(''))], check=True, timeout=10,
                         capture_output=True, text=True).stdout
    print(out.strip())
