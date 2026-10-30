/* Hard-link alias index (Task AX): shared attributes must reach every alias
 * row and only those, through link/unlink, row relocation, table growth,
 * reindex and whole-ino removal, while unrelated rows stay untouched. The
 * model is the names themselves: every name of a hard-linked ino is looked
 * up and must show the ino's current mode, size and nlink. */
#include "efs/metadata.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define TROOT EFS_ROOT_INO
#define H 5000
#define NFILES 600

static void expect(struct efs_export *ex, const char *name, uint32_t perm,
                   uint64_t size, uint32_t nlink)
{
    struct efs_inode in;
    if (efs_export_lookup(ex, TROOT, name, &in) != EFS_OK) {
        fprintf(stderr, "lookup %s failed\n", name);
        assert(0);
    }
    if ((in.mode & 07777) != perm || in.size != size || in.nlink != nlink) {
        fprintf(stderr, "%s: mode %o size %llu nlink %u, want %o %llu %u\n", name,
                in.mode & 07777, (unsigned long long)in.size, in.nlink, perm,
                (unsigned long long)size, nlink);
        assert(0);
    }
}

static void gone(struct efs_export *ex, const char *name)
{
    assert(efs_export_lookup(ex, TROOT, name, NULL) != EFS_OK);
}

static void file(struct efs_export *ex, efs_ino_t ino, const char *name)
{
    assert(efs_export_create_with_ino(ex, ino, TROOT, S_IFREG | 0644, 0, 0, name) == ino);
}

static void unrelated_untouched(struct efs_export *ex, int n)
{
    char nm[32];
    for (int i = 0; i < n; i++) {
        snprintf(nm, sizeof(nm), "f%d", i);
        if (efs_export_lookup(ex, TROOT, nm, NULL) == EFS_OK)
            expect(ex, nm, 0644, 0, 1);
    }
}

int main(void)
{
    struct efs_export ex;
    char nm[32];
    efs_export_init(&ex, 1, "t");

    for (int i = 0; i < NFILES; i++) {
        snprintf(nm, sizeof(nm), "f%d", i);
        file(&ex, 100 + i, nm);
        if (i == 3)
            file(&ex, H, "h0");
    }
    for (int k = 1; k <= 3; k++) {
        snprintf(nm, sizeof(nm), "h%d", k);
        assert(efs_export_link(&ex, H, TROOT, nm) == EFS_OK);
    }

    /* Attributes reach all four aliases; the unrelated rows are not written. */
    assert(efs_export_set_mode(&ex, H, S_IFREG | 0600) == EFS_OK);
    assert(efs_export_set_size(&ex, H, 4096) == EFS_OK);
    for (int k = 0; k <= 3; k++) {
        snprintf(nm, sizeof(nm), "h%d", k);
        expect(&ex, nm, 0600, 4096, 4);
    }
    unrelated_untouched(&ex, NFILES);

    /* Unlink an alias: the rest keep a shared, decremented nlink. */
    assert(efs_export_unlink_name_ex(&ex, TROOT, "h1", 0) == EFS_OK);
    gone(&ex, "h1");
    assert(efs_export_set_mode(&ex, H, S_IFREG | 0640) == EFS_OK);
    expect(&ex, "h0", 0640, 4096, 3);
    expect(&ex, "h2", 0640, 4096, 3);
    expect(&ex, "h3", 0640, 4096, 3);

    /* Removing an unrelated row swaps the last row (an alias) into its slot. */
    assert(efs_export_unlink_name_ex(&ex, TROOT, "f10", 0) == EFS_OK);
    assert(efs_export_set_mode(&ex, H, S_IFREG | 0660) == EFS_OK);
    assert(efs_export_set_size(&ex, H, 8192) == EFS_OK);
    for (int k = 0; k <= 3; k += (k == 0) ? 2 : 1) {
        snprintf(nm, sizeof(nm), "h%d", k);
        expect(&ex, nm, 0660, 8192, 3);
    }

    /* Forced table growth, then a link from a relocated row. */
    for (int i = 0; i < 2000; i++) {
        snprintf(nm, sizeof(nm), "g%d", i);
        file(&ex, 20000 + i, nm);
    }
    assert(efs_export_link(&ex, H, TROOT, "h4") == EFS_OK);
    assert(efs_export_set_mode(&ex, H, S_IFREG | 0600) == EFS_OK);
    for (const char *const *p = (const char *const[]){"h0", "h2", "h3", "h4", NULL}; *p; p++)
        expect(&ex, *p, 0600, 8192, 4);

    /* Reindex rebuilds the vector from the rows. */
    efs_export_compact(&ex);
    assert(efs_export_set_size(&ex, H, 1) == EFS_OK);
    for (const char *const *p = (const char *const[]){"h0", "h2", "h3", "h4", NULL}; *p; p++)
        expect(&ex, *p, 0600, 1, 4);

    /* Repeated grow/delete cycles of aliases keep every name in step. */
    for (int c = 0; c < 200; c++) {
        assert(efs_export_link(&ex, H, TROOT, "t1") == EFS_OK);
        assert(efs_export_link(&ex, H, TROOT, "t2") == EFS_OK);
        assert(efs_export_set_mode(&ex, H, S_IFREG | (c & 1 ? 0644 : 0600)) == EFS_OK);
        expect(&ex, "t1", c & 1 ? 0644 : 0600, 1, 6);
        expect(&ex, "t2", c & 1 ? 0644 : 0600, 1, 6);
        assert(efs_export_unlink_name_ex(&ex, TROOT, c & 1 ? "t1" : "t2", 0) == EFS_OK);
        assert(efs_export_unlink_name_ex(&ex, TROOT, c & 1 ? "t2" : "t1", 0) == EFS_OK);
        expect(&ex, "h4", c & 1 ? 0644 : 0600, 1, 4);
    }

    /* Down to one row: the last alias stays addressable and writable. */
    assert(efs_export_unlink_name_ex(&ex, TROOT, "h0", 0) == EFS_OK);
    assert(efs_export_unlink_name_ex(&ex, TROOT, "h2", 0) == EFS_OK);
    assert(efs_export_unlink_name_ex(&ex, TROOT, "h4", 0) == EFS_OK);
    expect(&ex, "h3", 0644, 1, 1);
    assert(efs_export_set_mode(&ex, H, S_IFREG | 0400) == EFS_OK);
    expect(&ex, "h3", 0400, 1, 1);
    assert(efs_export_link(&ex, H, TROOT, "h5") == EFS_OK);
    assert(efs_export_set_mode(&ex, H, S_IFREG | 0444) == EFS_OK);
    expect(&ex, "h3", 0444, 1, 2);
    expect(&ex, "h5", 0444, 1, 2);

    /* Whole-ino removal takes every alias and nothing else. */
    assert(efs_export_link(&ex, H, TROOT, "h6") == EFS_OK);
    assert(efs_export_unlink(&ex, H) == EFS_OK);
    gone(&ex, "h3");
    gone(&ex, "h5");
    gone(&ex, "h6");
    unrelated_untouched(&ex, NFILES);

    /* forget_ino drops every staged alias too. */
    file(&ex, H, "k0");
    assert(efs_export_link(&ex, H, TROOT, "k1") == EFS_OK);
    assert(efs_export_link(&ex, H, TROOT, "k2") == EFS_OK);
    efs_export_forget_ino(&ex, H);
    gone(&ex, "k0");
    gone(&ex, "k1");
    gone(&ex, "k2");
    unrelated_untouched(&ex, NFILES);

    efs_export_free(&ex);
    puts("test_hardlink_alias: ok");
    return 0;
}
