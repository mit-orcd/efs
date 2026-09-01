#include "efs/metadata.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static int failures = 0;

static void expect_str(const char *label, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got '%s' want '%s'\n", label, got, want);
        failures++;
    }
}

/* Build a v6 wire blob (contiguous dentry tail, version 6) from a table, to
 * verify the deserializer still reads the legacy layout. */
static char *build_v6_blob(struct efs_export *ex, size_t *out_len)
{
    size_t hdr = EFS_META_HDR_SIZE;
    size_t ino_bytes = (size_t)ex->inode_count * EFS_INODE_COMPACT_SIZE;
    size_t dent = (size_t)ex->dentry_bytes;
    size_t ch = (size_t)ex->chunk_count * EFS_CHUNK_WIRE_SIZE;
    size_t total = hdr + ino_bytes + dent + ch;
    uint8_t *b = calloc(1, total ? total : 1);
    if (!b)
        return NULL;
    efs_export_pack_header_ver(ex, b, EFS_META_EFSM_V6);
    uint8_t *p = b + hdr;
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        efs_export_pack_inode_compact(efs_export_inode_at(ex, i), p);
        p += EFS_INODE_COMPACT_SIZE;
    }
    for (uint64_t i = 0; i < ex->inode_count; i++) {
        uint16_t ln = (uint16_t)strnlen(efs_export_inode_name(ex, i),
                                        EFS_MAX_NAME - 1);
        memcpy(p, &ln, 2);
        p += 2;
        if (ln) {
            memcpy(p, efs_export_inode_name(ex, i), ln);
            p += ln;
        }
    }
    for (uint64_t i = 0; i < ex->chunk_count; i++)
        efs_export_pack_chunk(&ex->chunks[i], p + i * EFS_CHUNK_WIRE_SIZE);
    *out_len = total;
    return (char *)b;
}

int main(void)
{
    struct efs_export ex;
    efs_export_init(&ex, 7, "v6");
    efs_ino_t d = efs_export_create(&ex, EFS_ROOT_INO, S_IFDIR | 0755, 0, 0, "dir");
    efs_ino_t f = efs_export_create(&ex, d, S_IFREG | 0644, 0, 0, "short.jpg");
    if (!d || !f) {
        fprintf(stderr, "FAIL create\n");
        return 1;
    }
    efs_export_set_size(&ex, f, 1234);
    /* "/" + "dir" + "short.jpg" packed dentries */
    uint64_t want_dent = (2 + 1) + (2 + 3) + (2 + 9);
    if (ex.dentry_bytes != want_dent) {
        fprintf(stderr, "FAIL dentry_bytes %llu want %llu\n",
                (unsigned long long)ex.dentry_bytes,
                (unsigned long long)want_dent);
        failures++;
    }

    char *blob = NULL;
    size_t blen = 0;
    if (efs_export_serialize(&ex, &blob, &blen) != 0) {
        fprintf(stderr, "FAIL serialize\n");
        return 1;
    }
    uint32_t ver = 0;
    memcpy(&ver, blob + 4, 4);
    if (ver != EFS_META_EFSM_V7) {
        fprintf(stderr, "FAIL efsm version %u (want v7)\n", ver);
        failures++;
    }
    /* v7 page-aligns the dentry region: dentries start at the first page
     * boundary at/after HDR + inode_count*124. */
    size_t want_dent_off = efs_meta_dent_off(EFS_META_EFSM_V7, ex.inode_count);
    if (want_dent_off % EFS_META_PAGE_SIZE != 0) {
        fprintf(stderr, "FAIL dent_off %zu not page-aligned\n", want_dent_off);
        failures++;
    }
    if (blen != want_dent_off + ex.dentry_bytes +
                  ex.chunk_count * EFS_CHUNK_WIRE_SIZE) {
        fprintf(stderr, "FAIL v7 blob len %zu\n", blen);
        failures++;
    }
    /* v7 trades a <1-page dentry pad for O(1) creates, so a tiny table is
     * larger than v5's dense 420 B/inode blob; at scale the pad is noise.
     * Just assert the compact rows still beat v5 here (dentries excluded). */
    size_t v5 = (size_t)EFS_META_HDR_SIZE +
                ex.inode_count * EFS_INODE_WIRE_SIZE +
                ex.chunk_count * EFS_CHUNK_WIRE_SIZE;
    size_t compact_only = (size_t)EFS_META_HDR_SIZE +
                          ex.inode_count * EFS_INODE_COMPACT_SIZE +
                          ex.chunk_count * EFS_CHUNK_WIRE_SIZE;
    if (compact_only >= v5) {
        fprintf(stderr, "FAIL compact rows %zu not smaller than v5 %zu\n",
                compact_only, v5);
        failures++;
    }

    struct efs_export ex2;
    efs_export_init(&ex2, 0, "");
    if (efs_export_deserialize(&ex2, blob, blen) != 0) {
        fprintf(stderr, "FAIL deserialize\n");
        failures++;
    } else {
        struct efs_inode out;
        if (efs_export_lookup(&ex2, d, "short.jpg", &out) != 0) {
            fprintf(stderr, "FAIL lookup after v7 round-trip\n");
            failures++;
        } else {
            expect_str("name", out.name, "short.jpg");
            if (out.size != 1234) {
                fprintf(stderr, "FAIL size %llu\n",
                        (unsigned long long)out.size);
                failures++;
            }
        }
        if (ex2.efsm_version != EFS_META_EFSM_V7) {
            fprintf(stderr, "FAIL efsm_version %u (want v7)\n", ex2.efsm_version);
            failures++;
        }
        if (ex2.dentry_bytes != want_dent) {
            fprintf(stderr, "FAIL dentry_bytes after load %llu want %llu\n",
                    (unsigned long long)ex2.dentry_bytes,
                    (unsigned long long)want_dent);
            failures++;
        }
    }
    efs_export_free(&ex2);
    free(blob);

    /* Page-crossing round-trip: enough inodes to push the dentry region past
     * several compact-page boundaries (128 KiB / 124 B ≈ 1056 rows/page). */
    struct efs_export big;
    efs_export_init(&big, 9, "big");
    efs_ino_t bd = efs_export_create(&big, EFS_ROOT_INO, S_IFDIR | 0755, 0, 0, "bd");
    int nfiles = 3000;
    for (int i = 0; i < nfiles; i++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "file-%04d.dat", i);
        if (!efs_export_create(&big, bd, S_IFREG | 0644, 0, 0, nm)) {
            fprintf(stderr, "FAIL big create %d\n", i);
            failures++;
            break;
        }
    }
    char *bblob = NULL;
    size_t bblen = 0;
    if (efs_export_serialize(&big, &bblob, &bblen) != 0) {
        fprintf(stderr, "FAIL big serialize\n");
        failures++;
    } else {
        /* The dentry region must be page-aligned and at/after the v6
         * contiguous offset (the pad is by definition < 1 page). */
        size_t v6_off = (size_t)EFS_META_HDR_SIZE +
                        big.inode_count * EFS_INODE_COMPACT_SIZE;
        size_t v7_off = efs_meta_dent_off(EFS_META_EFSM_V7, big.inode_count);
        if (v7_off % EFS_META_PAGE_SIZE != 0 || v7_off < v6_off) {
            fprintf(stderr, "FAIL v7 dent_off %zu (v6 %zu) not aligned\n",
                    v7_off, v6_off);
            failures++;
        }
        struct efs_export big2;
        efs_export_init(&big2, 0, "");
        if (efs_export_deserialize(&big2, bblob, bblen) != 0) {
            fprintf(stderr, "FAIL big deserialize\n");
            failures++;
        } else {
            if (big2.inode_count != big.inode_count) {
                fprintf(stderr, "FAIL big inode_count %llu want %llu\n",
                        (unsigned long long)big2.inode_count,
                        (unsigned long long)big.inode_count);
                failures++;
            }
            /* Spot-check lookups across the whole range. */
            for (int i = 0; i < nfiles; i += 250) {
                char nm[64];
                snprintf(nm, sizeof(nm), "file-%04d.dat", i);
                struct efs_inode out;
                if (efs_export_lookup(&big2, bd, nm, &out) != 0) {
                    fprintf(stderr, "FAIL big lookup %s\n", nm);
                    failures++;
                }
            }
        }
        efs_export_free(&big2);
        free(bblob);
    }
    efs_export_free(&big);

    /* v6 read-compat: a hand-built legacy blob must still deserialize. */
    {
        size_t v6len = 0;
        char *v6 = build_v6_blob(&ex, &v6len);
        if (!v6) {
            fprintf(stderr, "FAIL build v6 blob\n");
            failures++;
        } else {
            struct efs_export ex6;
            efs_export_init(&ex6, 0, "");
            if (efs_export_deserialize(&ex6, v6, v6len) != 0) {
                fprintf(stderr, "FAIL v6 deserialize\n");
                failures++;
            } else {
                struct efs_inode out;
                if (efs_export_lookup(&ex6, d, "short.jpg", &out) != 0)
                    fprintf(stderr, "FAIL v6 lookup\n"), failures++;
                else
                    expect_str("v6 name", out.name, "short.jpg");
                if (ex6.efsm_version != EFS_META_EFSM_V6) {
                    fprintf(stderr, "FAIL v6 efsm_version %u\n",
                            ex6.efsm_version);
                    failures++;
                }
            }
            efs_export_free(&ex6);
            free(v6);
        }
    }

    char stats[1024];
    struct efs_inode root;
    efs_export_get_inode(&ex, EFS_ROOT_INO, &root);
    if (efs_export_format_stats_ex(&ex, &root, stats, sizeof(stats)) <= 0 ||
        !strstr(stats, "meta_ino_pages=") ||
        !strstr(stats, "meta_ino_pages_max=16384")) {
        fprintf(stderr, "FAIL stats:\n%s\n", stats);
        failures++;
    }

    /* Low-bits sharding: shard s owns ino == s (mod 2^bits); root -> 0. */
    if (efs_export_shard_of(1, 0) != 0 || efs_export_shard_of(EFS_ROOT_INO, 5) != 0 ||
        efs_export_shard_of(5, 2) != 1 || efs_export_shard_of(6, 2) != 2 ||
        efs_export_shard_of(8, 3) != 0 || efs_export_shard_of(9, 3) != 1) {
        fprintf(stderr, "FAIL shard_of\n");
        failures++;
    }
    /* Phase 3b: chunk groups spread independently of the inode shard. */
    if (efs_export_chunk_shard_of(9, 0, 0) != 0 ||
        efs_export_chunk_shard_of(9, 0, 3) != efs_export_shard_of(9, 3)) {
        fprintf(stderr, "FAIL chunk_shard_of identity-at-group0\n");
        failures++;
    }
    {
        uint32_t seen = 0;
        for (uint32_t g = 0; g < 32; g++) {
            uint32_t ci = g << EFS_CHUNK_GROUP_SHIFT;
            uint32_t sh = efs_export_chunk_shard_of(9, ci, 3);
            if (sh >= 8) {
                fprintf(stderr, "FAIL chunk_shard_of range %u\n", sh);
                failures++;
                break;
            }
            seen |= 1u << sh;
        }
        if (seen == (1u << efs_export_shard_of(9, 3))) {
            fprintf(stderr, "FAIL chunk_shard_of did not spread groups\n");
            failures++;
        }
        if (efs_export_chunk_shard_of(9, 0, 3) !=
            efs_export_chunk_shard_of(9, EFS_CHUNK_GROUP_SIZE - 1, 3)) {
            fprintf(stderr, "FAIL chunk_shard_of not constant in group\n");
            failures++;
        }
        if (!efs_inode_dir_is_spread(&(struct efs_inode){
                .imm_files = EFS_DIR_SPREAD_MIN, .imm_dirs = 0}) ||
            efs_inode_dir_is_spread(&(struct efs_inode){
                .imm_files = EFS_DIR_SPREAD_MIN - 1, .imm_dirs = 0})) {
            fprintf(stderr, "FAIL dir_is_spread threshold\n");
            failures++;
        }
    }
    if (efs_meta_shard_table_ino(0) != EFS_META_TABLE_INO ||
        efs_meta_shard_table_ino(3) != EFS_META_TABLE_INO + 3) {
        fprintf(stderr, "FAIL shard table ino\n");
        failures++;
    }
    {
        efs_node_id_t live[4] = {2, 4, 1, 3};
        efs_node_id_t live2[4] = {1, 2, 3, 4};
        /* Canonical (sorted) ownership: input order must not matter. */
        if (efs_shard_owner_of(0, 1, live, 4) != 1 ||
            efs_shard_owner_of(0, 0, live, 4) != 1 ||
            efs_shard_owner_of(5, 8, live, 4) != 2 ||
            efs_shard_owner_of(5, 8, live2, 4) != 2 ||
            efs_shard_owner_of(1, 8, live, 4) != 2 ||
            efs_shard_owner_of(4, 8, live, 4) != 1 ||
            efs_shard_owner_of(0, 8, NULL, 0) != 0) {
            fprintf(stderr, "FAIL shard_owner_of\n");
            failures++;
        }
    }
    {
        struct efs_export sh;
        efs_export_init(&sh, 1, "shard-alloc");
        sh.root.shard_bits = 8;
        sh.root.shard_count = 256;
        sh.next_ino = 2;
        /* Main table (shard 0): class 0 — 0 invalid and 1 is the root,
         * so the first allocatable ino is 256, then 512. */
        efs_ino_t a = efs_export_alloc_ino(&sh, EFS_ROOT_INO);
        efs_ino_t b = efs_export_alloc_ino(&sh, EFS_ROOT_INO);
        if (a != 256 || b != 512 ||
            efs_export_shard_of(a, 8) != 0 ||
            efs_export_shard_of(b, 8) != 0) {
            fprintf(stderr, "FAIL alloc_ino shard0: a=%llu b=%llu\n",
                    (unsigned long long)a, (unsigned long long)b);
            failures++;
        }
        /* Shard-1 table: class 1 — skips 1 (root), first free is 257. */
        struct efs_export *t1 = efs_export_table(&sh, 1);
        efs_ino_t c = t1 ? efs_export_alloc_ino(t1, 0) : 0;
        if (c != 257 || efs_export_shard_of(c, 8) != 1) {
            fprintf(stderr, "FAIL alloc_ino shard1: c=%llu\n",
                    (unsigned long long)c);
            failures++;
        }
        efs_export_free(&sh);
    }
    {
        struct efs_export sh;
        efs_export_init(&sh, 2, "two-shard");
        sh.root.shard_bits = 8;
        sh.root.shard_count = 256;
        struct efs_export *t0 = efs_export_table_for_ino(&sh, EFS_ROOT_INO);
        struct efs_export *t1 = efs_export_table(&sh, 1);
        if (t0 != &sh || !t1 || t1 == &sh) {
            fprintf(stderr, "FAIL table_for_ino split\n");
            failures++;
        } else {
            /* A class-1 parent dir (ino 257) whose row lives on shard 1. */
            efs_ino_t pdir = efs_export_create_with_ino(t1, 257, EFS_ROOT_INO,
                                                        S_IFDIR | 0755,
                                                        0, 0, "pdir");
            efs_ino_t child = efs_export_create(t1, 257, S_IFREG | 0644,
                                                0, 0, "in-shard-1");
            struct efs_inode got;
            if (!pdir || !child || child == 257 ||
                efs_export_shard_of(child, 8) != 1 ||
                efs_export_lookup(t1, 257, "in-shard-1", &got) != 0 ||
                efs_export_lookup(&sh, 257, "in-shard-1", &got) != 0 ||
                efs_export_table_for_ino(&sh, child) != t1) {
                fprintf(stderr, "FAIL two-shard create/lookup child=%llu\n",
                        (unsigned long long)child);
                failures++;
            }
            if (!efs_ino_is_meta_table(efs_meta_shard_table_ino(0)) ||
                !efs_ino_is_meta_table(efs_meta_shard_table_ino(3)) ||
                efs_ino_is_meta_table(EFS_ROOT_INO)) {
                fprintf(stderr, "FAIL efs_ino_is_meta_table\n");
                failures++;
            }
        }
        efs_export_free(&sh);
    }
    {
        struct efs_export sh;
        efs_export_init(&sh, 3, "v8-root");
        char *blob = NULL;
        size_t blen = 0;
        uint32_t ino_len = 0, ch_len = 0;
        if (efs_export_serialize_ex(&sh, &blob, &blen, &ino_len, &ch_len) != 0) {
            fprintf(stderr, "FAIL v8 serialize table\n");
            failures++;
        } else {
            struct efs_export_root r;
            memset(&r, 0, sizeof(r));
            if (efs_export_root_prepare(&r, &sh, 7, ino_len, ch_len) != 0) {
                fprintf(stderr, "FAIL v8 prepare\n");
                failures++;
            } else {
                char *rbuf = NULL;
                size_t rlen = 0;
                struct efs_export_root back;
                memset(&back, 0, sizeof(back));
                if (r.version != EFS_META_ROOT_VERSION_V8 ||
                    efs_export_root_serialize(&r, &rbuf, &rlen) != 0 ||
                    efs_export_root_deserialize(&back, rbuf, rlen) != 0 ||
                    back.version != EFS_META_ROOT_VERSION_V8 ||
                    back.generation != 7 ||
                    back.extra_shard_count != 0) {
                    fprintf(stderr, "FAIL v8 roundtrip ver=%u extra=%u gen=%llu\n",
                            back.version, back.extra_shard_count,
                            (unsigned long long)back.generation);
                    failures++;
                }
                efs_export_root_free(&back);
                free(rbuf);
            }
            efs_export_root_free(&r);
        }
        free(blob);
        efs_export_free(&sh);
    }
    {
        struct efs_export sh, snap;
        efs_export_init(&sh, 3, "snap");
        efs_ino_t f = efs_export_create(&sh, EFS_ROOT_INO, S_IFREG | 0644,
                                        0, 0, "snapf");
        efs_node_id_t nodes[EFS_NUM_FRAGMENTS] = {1, 2, 3};
        uint8_t cks[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        memset(cks, 0xab, sizeof(cks));
        if (!f || efs_export_set_chunk(&sh, f, 0, nodes, cks) != 0 ||
            efs_export_table_snapshot(&sh, &snap) != 0) {
            fprintf(stderr, "FAIL table snapshot\n");
            failures++;
        } else {
            char *a = NULL, *b = NULL;
            size_t la = 0, lb = 0;
            if (efs_export_serialize(&sh, &a, &la) != 0 ||
                efs_export_serialize(&snap, &b, &lb) != 0 ||
                la != lb || memcmp(a, b, la) != 0) {
                fprintf(stderr, "FAIL snapshot serialize mismatch\n");
                failures++;
            }
            free(a);
            free(b);
            efs_export_table_snapshot_free(&snap);
        }
        efs_export_free(&sh);
    }
    {
        struct efs_export sh;
        efs_export_init(&sh, 4, "spread");
        sh.root.shard_bits = 3;
        sh.root.shard_count = 8;
        sh.create_stride = 1;
        efs_ino_t a = efs_export_create(&sh, EFS_ROOT_INO, S_IFREG | 0644,
                                        0, 0, "a");
        efs_ino_t b = efs_export_create(&sh, EFS_ROOT_INO, S_IFREG | 0644,
                                        0, 0, "b");
        efs_ino_t d = efs_export_create(&sh, EFS_ROOT_INO, S_IFDIR | 0755,
                                        0, 0, "d");
        struct efs_inode ga, gb, gd;
        if (!a || !b || !d ||
            efs_export_lookup(&sh, EFS_ROOT_INO, "a", &ga) != 0 ||
            efs_export_lookup(&sh, EFS_ROOT_INO, "b", &gb) != 0 ||
            efs_export_lookup(&sh, EFS_ROOT_INO, "d", &gd) != 0 ||
            !efs_mode_is_dir(gd.mode) || gd.ino != d ||
            ga.ino != a || gb.ino != b) {
            fprintf(stderr, "FAIL spread create/lookup a=%llu b=%llu d=%llu\n",
                    (unsigned long long)a, (unsigned long long)b,
                    (unsigned long long)d);
            failures++;
        }
        /* Files stay on the parent shard; only ROOT directories hash. The old
         * create_stride round-robin is gone, so a and b may share shard 0. */
        uint32_t loaded_before = 0;
        for (uint32_t i = 1; sh.shard_tabs && i < sh.shard_tab_cap; i++)
            if (sh.shard_tabs[i])
                loaded_before++;
        if (efs_export_unlink_name(&sh, EFS_ROOT_INO, "a") != 0 ||
            efs_export_lookup(&sh, EFS_ROOT_INO, "a", NULL) == 0) {
            fprintf(stderr, "FAIL spread unlink\n");
            failures++;
        }
        uint32_t loaded_after = 0;
        for (uint32_t i = 1; sh.shard_tabs && i < sh.shard_tab_cap; i++)
            if (sh.shard_tabs[i])
                loaded_after++;
        if (loaded_after > loaded_before) {
            fprintf(stderr, "FAIL unlink instantiated extras %u -> %u\n",
                    loaded_before, loaded_after);
            failures++;
        }
        efs_export_free(&sh);
    }
    {
        /* Parent-shard dentries must exist for off-shard file inodes.
         * create_with_ino used to efs_export_lookup the sharded root and
         * see the child-tab row, skipping the parent copy. */
        struct efs_export sh;
        efs_export_init(&sh, 6, "pdent");
        sh.root.shard_bits = 3;
        sh.root.shard_count = 8;
        sh.create_stride = 1;
        uint32_t on_parent = 0;
        for (int i = 0; i < 8; i++) {
            char n[8];
            snprintf(n, sizeof(n), "p%d", i);
            if (!efs_export_create(&sh, EFS_ROOT_INO, S_IFREG | 0644, 0, 0, n))
                failures++;
        }
        for (uint64_t i = 0; i < sh.inode_count; i++) {
            const struct efs_inode_mem *row = efs_export_inode_at(&sh, i);
            if (row && row->parent == EFS_ROOT_INO &&
                row->ino != EFS_ROOT_INO &&
                efs_export_inode_name(&sh, i)[0] == 'p')
                on_parent++;
        }
        if (on_parent != 8) {
            fprintf(stderr, "FAIL parent dentries %u want 8\n", on_parent);
            failures++;
        }
        /* Child-tab rows keep the create name; lookup must not see it
         * after the parent dentry is renamed/unlinked. */
        if (efs_export_rename_at(&sh, EFS_ROOT_INO, "p0", EFS_ROOT_INO, "q0") !=
                0 ||
            efs_export_lookup(&sh, EFS_ROOT_INO, "p0", NULL) == EFS_OK) {
            fprintf(stderr, "FAIL lookup ghost after rename p0\n");
            failures++;
        }
        if (efs_export_lookup(&sh, EFS_ROOT_INO, "q0", NULL) != EFS_OK) {
            fprintf(stderr, "FAIL lookup q0 after rename\n");
            failures++;
        }
        if (efs_export_unlink_name(&sh, EFS_ROOT_INO, "p1") != 0 ||
            efs_export_lookup(&sh, EFS_ROOT_INO, "p1", NULL) == EFS_OK) {
            fprintf(stderr, "FAIL lookup ghost after unlink p1\n");
            failures++;
        }
        efs_export_free(&sh);
    }
    {
        struct efs_export sh;
        efs_export_init(&sh, 5, "rehash");
        sh.next_ino = 20; /* ino 20 lands on shard 20 & 7 = 4 after bits=3 */
        efs_ino_t f = efs_export_create(&sh, EFS_ROOT_INO, S_IFREG | 0644,
                                        0, 0, "f");
        if (!f || efs_export_rehash(&sh, 3) != 0 ||
            sh.root.shard_bits != 3 || sh.root.shard_count != 8) {
            fprintf(stderr, "FAIL rehash bits\n");
            failures++;
        } else {
            struct efs_inode got;
            if (efs_export_lookup(&sh, EFS_ROOT_INO, "f", &got) != 0 ||
                got.ino != f) {
                fprintf(stderr, "FAIL rehash lookup f=%llu\n",
                        (unsigned long long)f);
                failures++;
            }
            efs_export_evict_cold_shards(&sh, 1);
            if (efs_export_load_shard(&sh, efs_export_shard_of(f, 3)) != 0) {
                fprintf(stderr, "FAIL load_shard after evict\n");
                failures++;
            }
        }
        efs_export_free(&sh);
    }
    {
        /* Stale-low dentry_bytes used to undersize the serialize blob and
         * smash the next malloc chunk (flush SIGABRT). */
        struct efs_export st;
        char *sb = NULL;
        size_t sl = 0;
        efs_export_init(&st, 1, "stale-dent");
        if (!efs_export_create(&st, EFS_ROOT_INO, S_IFREG | 0644, 0, 0,
                               "longish-name")) {
            fprintf(stderr, "FAIL stale-dent create\n");
            failures++;
        } else {
            st.dentry_bytes = 1;
            if (efs_export_serialize(&st, &sb, &sl) != 0 || !sb) {
                fprintf(stderr, "FAIL stale-dent serialize\n");
                failures++;
            }
            free(sb);
        }
        efs_export_free(&st);
    }
    {
        /* Incremental serialize of a create+setattr must match a full pack. */
        struct efs_export st;
        char *cache = NULL, *full = NULL, *incr = NULL;
        size_t cl = 0, fl = 0, il = 0;
        uint32_t cino = 0, cch = 0, fino = 0, fch = 0, iino = 0, ich = 0;
        int used = 0;
        efs_export_init(&st, 1, "incr");
        for (int i = 0; i < 2000; i++) {
            char n[32];
            snprintf(n, sizeof(n), "f%04d", i);
            if (!efs_export_create(&st, EFS_ROOT_INO, S_IFREG | 0644, 0, 0, n)) {
                fprintf(stderr, "FAIL incr create %d\n", i);
                failures++;
                break;
            }
        }
        if (efs_export_serialize_ex(&st, &cache, &cl, &cino, &cch) != 0) {
            fprintf(stderr, "FAIL incr cache serialize\n");
            failures++;
        } else {
            efs_export_flush_clear_dirty(&st);
            if (!efs_export_create(&st, EFS_ROOT_INO, S_IFREG | 0644, 0, 0,
                                   "tail")) {
                fprintf(stderr, "FAIL incr tail create\n");
                failures++;
            }
            efs_export_set_size(&st, efs_export_inode_at(&st, 1)->ino, 999);
            if (efs_export_serialize_ex(&st, &full, &fl, &fino, &fch) != 0) {
                fprintf(stderr, "FAIL incr full serialize\n");
                failures++;
            } else if (efs_export_serialize_dirty(&st, cache, cino, cch, 0,
                                                 &incr, &il, &iino, &ich,
                                                 &used) != 0 ||
                       !used || !incr) {
                fprintf(stderr, "FAIL incr dirty serialize used=%d\n", used);
                failures++;
            } else if (fl != il || fino != iino || fch != ich ||
                       memcmp(full, incr, fl) != 0) {
                fprintf(stderr, "FAIL incr mismatch full=%zu incr=%zu\n",
                        fl, il);
                failures++;
            }
            free(full);
            free(incr);
            full = incr = NULL;
            efs_export_flush_clear_dirty(&st);
            if (efs_export_rename(&st, efs_export_inode_at(&st, 1)->ino, EFS_ROOT_INO,
                                  "renamed") != 0) {
                fprintf(stderr, "FAIL incr rename\n");
                failures++;
            } else if (efs_export_serialize_ex(&st, &full, &fl, &fino,
                                              &fch) != 0 ||
                       efs_export_serialize_dirty(&st, cache, cino, cch, 0,
                                                  &incr, &il, &iino, &ich,
                                                  &used) != 0 ||
                       !used || fl != il || memcmp(full, incr, fl) != 0) {
                fprintf(stderr, "FAIL incr rename serialize used=%d\n", used);
                failures++;
            }
        }
        free(cache);
        free(full);
        free(incr);
        efs_export_free(&st);
    }

    efs_export_free(&ex);
    if (failures) {
        printf("test_meta_v6: %d failures\n", failures);
        return 1;
    }
    printf("test_meta_v6: OK\n");
    return 0;
}
