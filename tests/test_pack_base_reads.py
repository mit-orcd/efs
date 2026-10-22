#!/usr/bin/env python3
"""Packed bases are read a segment at a time: files whose base records share a segment cost one segment read and one validation, each file still gets its own record checked, and a failed segment fails only its own files."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/packer.c').read_text()
def fn(sig,end='\n}\n'):
    a=s.index(sig);return s[a:s.index(end,a)+len(end)]
assert 'pk_read_bases(nodes, count, files, n, flen);' in s, 'the pass reads packed bases before its files'
assert 'pk_read_packed_base(' not in s, 'no per-file segment read remains'
code=r'''
#include "efs/pack_mapping.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PK_READ_THREADS 4u
static struct { struct { uint64_t base_segments, base_files; } st; } pk;
#define STAT_ADD(f, n) __atomic_fetch_add(&pk.st.f, (n), __ATOMIC_RELAXED)
static int reads, fail_id;
static struct efs_kv *server_raft_host_kv(void) { return (struct efs_kv *)1; }
int efs_pack_object_get(struct efs_kv *kv, uint32_t g, uint64_t id, uint64_t gen, struct efs_pack_object *o)
{ (void)kv; memset(o, 0, sizeof(*o)); o->ledger.publication_group = g; o->ledger.segment_id = id; o->ledger.segment_generation = gen; return EFS_OK; }
static struct efs_pack_record rec[8]; static uint8_t bytes[3][8][16]; static struct efs_pack_ref refs[3][8];
static int pk_read_segment(const struct efs_node *nodes, uint32_t count, const struct efs_pack_object *o,
                           uint8_t *image, struct efs_pack_view *view)
{
    (void)nodes; (void)count; size_t g; uint64_t id = o->ledger.segment_id;
    struct efs_pack_record rec[8]; /* per call: the runs read on several threads */
    __atomic_fetch_add(&reads, 1, __ATOMIC_RELAXED);
    if ((int)id == fail_id) return EFS_ERR_IO;
    for (unsigned i = 0; i < 8; i++) rec[i] = (struct efs_pack_record){.ino = 100 * id + i, .inode_generation = 1,
        .record_generation = 1 + i, .len = 16, .data = bytes[id][i]};
    struct efs_pack_ref r[8];
    assert(!efs_pack_build(id, 1, 0, rec, 8, image, EFS_SEGMENT_MAX_SIZE, &g, r));
    return efs_pack_view_open(image, g, view);
}
'''+fn('struct pk_file {','\n};\n')+fn('struct pk_base_key {','};\n')+fn('static int pk_base_cmp(')+fn('struct pk_bases {','\n};\n')+fn('static void pk_base_run(')+fn('static void *pk_bases_run(')+fn('static void pk_read_bases(')+r'''
int main(void)
{
    /* Two segments with eight records each; reference builds for the files. */
    for (unsigned id = 1; id <= 2; id++) {
        uint8_t *image = malloc(EFS_SEGMENT_MAX_SIZE); size_t g;
        for (unsigned i = 0; i < 8; i++) {
            memset(bytes[id][i], (int)(id * 16 + i), 16);
            rec[i] = (struct efs_pack_record){.ino = 100 * id + i, .inode_generation = 1,
                .record_generation = 1 + i, .len = 16, .data = bytes[id][i]};
        }
        assert(!efs_pack_build(id, 1, 0, rec, 8, image, EFS_SEGMENT_MAX_SIZE, &g, refs[id]));
        free(image);
    }
    /* Ten files, interleaved across the two segments, plus two unpacked ones. */
    struct pk_file files[12]; memset(files, 0, sizeof(files));
    for (unsigned x = 0; x < 10; x++) {
        unsigned id = 1 + x % 2, i = x / 2;
        files[x].c.base.coding_profile_id = EFS_META_PROFILE_PACK;
        files[x].c.base.packed = refs[id][i];
        files[x].c.size = 16;
    }
    files[10].c.base.coding_profile_id = EFS_META_PROFILE_K2F1; files[10].c.size = 16;
    files[11].c.base.coding_profile_id = EFS_META_PROFILE_K2F1; files[11].c.size = 16;
    files[3].c.base.packed.record_generation++;   /* a stale reference: refused alone */
    uint64_t v0 = efs_pack_validations();
    pk_read_bases(NULL, 0, files, 12, 4096);
    assert(reads == 2 && efs_pack_validations() - v0 == 2);   /* one per segment, not per file */
    assert(pk.st.base_segments == 2 && pk.st.base_files == 10);
    for (unsigned x = 0; x < 10; x++) {
        unsigned id = 1 + x % 2, i = x / 2;
        assert(files[x].base_read && files[x].data);
        if (x == 3) { assert(files[x].base_rc == EFS_ERR_STALE); continue; }
        assert(files[x].base_rc == EFS_OK && !memcmp(files[x].data, bytes[id][i], 16));
    }
    assert(!files[10].base_read && !files[11].base_read && !files[10].data);
    for (unsigned x = 0; x < 12; x++) { free(files[x].data); files[x].data = NULL; files[x].base_read = 0; }
    files[3].c.base.packed.record_generation--;
    /* A segment that cannot be read fails its own files only. */
    reads = 0; fail_id = 2;
    pk_read_bases(NULL, 0, files, 12, 4096);
    assert(reads == 2);
    for (unsigned x = 0; x < 10; x++)
        assert(files[x].base_read && (x % 2 ? files[x].base_rc == EFS_ERR_IO : files[x].base_rc == EFS_OK));
    for (unsigned x = 0; x < 12; x++) free(files[x].data);
    puts("pack bases: one segment read and validation per segment, per-file reference checks, failure isolation PASS");
    return 0;
}
'''
srcs=['src/common/packed_segment.c','src/common/pack_ledger.c','src/common/checksum.c','src/data/erasure.c',
      'deps/blake3/blake3.c','deps/blake3/blake3_dispatch.c','deps/blake3/blake3_portable.c']
with tempfile.TemporaryDirectory(prefix='efs-pack-bases-') as d:
    c=os.path.join(d,'t.c');x=os.path.join(d,'t');open(c,'w').write(code)
    subprocess.check_call([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Werror','-D_GNU_SOURCE',
        '-DBLAKE3_NO_SSE2','-DBLAKE3_NO_SSE41','-DBLAKE3_NO_AVX2','-DBLAKE3_NO_AVX512','-DBLAKE3_USE_NEON=0',
        '-I'+str(root/'include'),'-I'+str(root/'src/common'),'-I'+str(root/'deps/blake3'),'-o',x,c]+
        [str(root/p) for p in srcs]+['-lpthread','-lm'])
    subprocess.check_call([x])
