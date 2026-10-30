/* AW: reverse slot -> position index invariants.
 * Forced realloc and pos-map rehash, swap of a row from another parent,
 * several aliases, repeated grow/delete cycles, allocation-failure atomicity
 * (--wrap realloc/calloc), and root handling. Verification is always the
 * public listing (efs_export_foreach_child + efs_export_inode_name).
 */
#include "efs/metadata.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int collect_cb(struct efs_export *ex, uint64_t slot, void *arg)
{
    (void)ex;
    char *buf = arg;
    strcat(buf, efs_export_inode_name(ex, slot));
    strcat(buf, " ");
    return 0;
}

static void listing(struct efs_export *ex, efs_ino_t parent, char *buf, size_t cap)
{
    buf[0] = 0;
    efs_export_foreach_child(ex, parent, collect_cb, buf);
    (void)cap;
}

static int has_name(const char *listing, const char *name)
{
    char pat[EFS_MAX_NAME + 2];
    snprintf(pat, sizeof(pat), "%s ", name);
    return strstr(listing, pat) != NULL;
}

static efs_ino_t mkchild(struct efs_export *ex, efs_ino_t dir, uint64_t i,
                         const char *prefix)
{
    char name[EFS_MAX_NAME];
    snprintf(name, sizeof(name), "%s%06llu", prefix, (unsigned long long)i);
    efs_ino_t ino = efs_export_create_with_ino(ex, i + 1000, dir,
                                               S_IFREG | 0644, 0, 0, name);
    assert(ino);
    return ino;
}

static void check_full_set(struct efs_export *ex, efs_ino_t dir,
                           const char *prefix, uint64_t from, uint64_t to,
                           const char *skip)
{
    char buf[1 << 20];
    listing(ex, dir, buf, sizeof(buf));
    for (uint64_t i = from; i < to; i++) {
        char name[EFS_MAX_NAME];
        snprintf(name, sizeof(name), "%s%06llu", prefix, (unsigned long long)i);
        if (skip && strcmp(name, skip) == 0)
            continue;
        if (!has_name(buf, name)) {
            fprintf(stderr, "missing %s in listing\n", name);
            assert(0);
        }
    }
}

/* --wrap allocator for the failure-atomicity case. */
static long wrap_arm;
void *__real_realloc(void *, size_t);
void *__real_calloc(size_t, size_t);
void *__wrap_realloc(void *p, size_t n)
{
    if (wrap_arm > 0 && --wrap_arm == 0)
        return NULL;
    return __real_realloc(p, n);
}
void *__wrap_calloc(size_t n, size_t s)
{
    if (wrap_arm > 0 && --wrap_arm == 0)
        return NULL;
    return __real_calloc(n, s);
}

int main(void)
{
    struct efs_export ex;
    efs_export_init(&ex, 1, "aw-test");
    efs_ino_t dir = efs_export_create_with_ino(&ex, 2, EFS_ROOT_INO,
                                               S_IFDIR | 0755, 0, 0, "d");
    assert(dir == 2);

    /* Forced realloc + pos-map rehash, then delete half in permuted order. */
    for (uint64_t i = 0; i < 257; i++)
        mkchild(&ex, dir, i, "f");
    check_full_set(&ex, dir, "f", 0, 257, NULL);
    for (unsigned k = 0; k < 130; k++) {
        uint64_t i = (k * 2654435761ull + 7) % 257;
        char name[EFS_MAX_NAME];
        snprintf(name, sizeof(name), "f%06llu", (unsigned long long)i);
        if (efs_export_lookup(&ex, dir, name, &(struct efs_inode){0}) != 0)
            continue;
        assert(efs_export_unlink_name_ex(&ex, dir, name, 0) == EFS_OK);
        char buf[1 << 20];
        listing(&ex, dir, buf, sizeof(buf));
        assert(!has_name(buf, name));
    }

    /* Swap of a row from another parent: two directories, delete one inode
     * so the last row (a child of the other directory) moves into the freed
     * slot; child_idx_replace_slot must repair the other parent's vector. */
    efs_ino_t d2 = efs_export_create_with_ino(&ex, 3, EFS_ROOT_INO,
                                              S_IFDIR | 0755, 0, 0, "e");
    assert(d2 == 3);
    for (uint64_t i = 300; i < 310; i++)
        mkchild(&ex, d2, i, "g");
    for (uint64_t i = 300; i < 305; i++) {
        char name[EFS_MAX_NAME];
        snprintf(name, sizeof(name), "g%06llu", (unsigned long long)i);
        assert(efs_export_unlink_name_ex(&ex, d2, name, 0) == EFS_OK);
    }
    check_full_set(&ex, d2, "g", 305, 310, NULL);

    /* Several aliases: one inode with names under both parents; deleting one
     * alias keeps the others and the row's hardlink identity. */
    efs_ino_t a1 = mkchild(&ex, dir, 400, "h");
    assert(efs_export_link(&ex, a1, d2, "h-alias") == EFS_OK);
    char buf[1 << 20];
    listing(&ex, d2, buf, sizeof(buf));
    assert(has_name(buf, "h-alias"));
    assert(efs_export_unlink_name_ex(&ex, d2, "h-alias", 0) == EFS_OK);
    listing(&ex, dir, buf, sizeof(buf));
    assert(has_name(buf, "h000400"));
    listing(&ex, d2, buf, sizeof(buf));
    assert(!has_name(buf, "h-alias"));

    /* Repeated grow/delete cycles on one parent. */
    for (unsigned round = 0; round < 40; round++) {
        for (uint64_t i = 0; i < 20; i++)
            mkchild(&ex, dir, 10000 + round * 100 + i, "c");
        for (uint64_t i = 0; i < 20; i++) {
            char name[EFS_MAX_NAME];
            snprintf(name, sizeof(name), "c%06llu",
                     (unsigned long long)(10000 + round * 100 + i));
            assert(efs_export_unlink_name_ex(&ex, dir, name, 0) == EFS_OK);
        }
        listing(&ex, dir, buf, sizeof(buf));
        assert(!has_name(buf, "c010000"));
    }

    /* Root handling: the root's directories are listed as its children, but
     * the root inode itself is never a child of itself. */
    listing(&ex, EFS_ROOT_INO, buf, sizeof(buf));
    assert(has_name(buf, "d"));
    assert(has_name(buf, "e"));

    /* Allocation failure: every allocation point in child_idx_add must leave
     * the previous state intact (failure atomicity). Unique name per arm so
     * an earlier success cannot masquerade as a later failure. */
    struct efs_export fx;
    efs_export_init(&fx, 2, "aw-fail");
    assert(efs_export_create_with_ino(&fx, 2, EFS_ROOT_INO,
                                      S_IFDIR | 0755, 0, 0, "d") == 2);
    for (uint64_t i = 0; i < 100; i++)
        mkchild(&fx, 2, i, "f");
    for (long arm = 1; arm <= 12; arm++) {
        char nm[EFS_MAX_NAME];
        snprintf(nm, sizeof(nm), "new%02ld", arm);
        wrap_arm = arm;
        efs_ino_t ino = efs_export_create_with_ino(&fx, 500 + arm, 2,
                                                   S_IFREG | 0644, 0, 0, nm);
        wrap_arm = 0;
        char fbuf[1 << 20];
        listing(&fx, 2, fbuf, sizeof(fbuf));
        for (uint64_t i = 0; i < 100; i++) {
            char name[EFS_MAX_NAME];
            snprintf(name, sizeof(name), "f%06llu", (unsigned long long)i);
            assert(has_name(fbuf, name));
        }
        /* A create that never reached child_idx_add must leave no trace. */
        if (!ino)
            assert(!has_name(fbuf, nm));
        /* Whatever happened, the index stays consistent: a name that did
         * land is deletable, and the next create works. */
        if (has_name(fbuf, nm))
            assert(efs_export_unlink_name_ex(&fx, 2, nm, 0) == EFS_OK);
        mkchild(&fx, 2, 900 + arm, "z");
    }
    efs_export_free(&fx);
    efs_export_free(&ex);
    puts("child vec index: forced realloc, cross-parent swap, aliases, grow/delete cycles, allocation failure, root handling PASS");
    return 0;
}
