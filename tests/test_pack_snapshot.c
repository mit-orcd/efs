#define _XOPEN_SOURCE 700
#include "efs/kv_snap.h"
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int cleanup(const char *p, const struct stat *s, int t, struct FTW *f)
{ (void)s; (void)t; (void)f; return remove(p); }
static void marked(const char *dir)
{
    char path[1024], line[128]; snprintf(path, sizeof(path), "%s/MANIFEST", dir);
    FILE *f = fopen(path, "r"); assert(f && fgets(line, sizeof(line), f));
    assert(!strncmp(line, "KVLSM2 ", 7)); assert(!fclose(f));
}
static void file(const char *path, const uint8_t *p, uint32_t n)
{ FILE *f = fopen(path, "wb"); assert(f && fwrite(p, 1, n, f) == n); assert(!fclose(f)); }
int main(void)
{
    char dir[] = "/tmp/efs-pack-snapshot-XXXXXX"; assert(mkdtemp(dir));
    char dest[1024], in[1024], local[1024];
    snprintf(in, sizeof(in), "%s/incoming", dir); snprintf(local, sizeof(local), "%s/local", dir);
    struct efs_kv *source = efs_kv_mem_create(); assert(source);
    const uint8_t key[] = {0, 1, EFS_KV_KIND_PACK_READER}, value[] = {0, 0, 0, 1};
    assert(!efs_kv_put(source, key, sizeof(key), value, sizeof(value)));
    uint8_t *image; uint32_t len; assert(!efs_kv_group_export(source, 0, 4096, &image, &len));
    file(in, image, len); file(local, image, len);
    snprintf(dest, sizeof(dest), "%s/direct", dir);
    struct efs_kv *kv = efs_kv_lsm_open(dest, NULL); assert(kv);
    assert(!efs_kv_group_import(kv, 0, image, len)); marked(dest);
    efs_kv_lsm_close(kv); kv = efs_kv_lsm_open(dest, NULL); assert(kv); marked(dest); efs_kv_lsm_close(kv);
    /* No diff still carries a format requirement. It must mark the actual
     * receiving store before the background result can be applied. */
    snprintf(dest, sizeof(dest), "%s/diff", dir); kv = efs_kv_lsm_open(dest, NULL); assert(kv);
    struct efs_kv_item *diff; uint32_t n; uint8_t *a, *b;
    assert(!efs_kv_group_import_prepare(kv, 0, in, local, &diff, &n, &a, &b));
    assert(!n); marked(dest); free(diff); free(a); free(b); efs_kv_lsm_close(kv);
    /* An unsupported capability fails before rows are installed. */
    uint8_t bad[] = {0, 0, 0, 2}; assert(!efs_kv_put(source, key, sizeof(key), bad, sizeof(bad)));
    free(image); assert(!efs_kv_group_export(source, 0, 4096, &image, &len)); file(in, image, len);
    snprintf(dest, sizeof(dest), "%s/reject", dir); kv = efs_kv_lsm_open(dest, NULL); assert(kv);
    assert(efs_kv_group_import(kv, 0, image, len) == EFS_ERR_PROTO);
    assert(efs_kv_group_import_prepare(kv, 0, in, local, &diff, &n, &a, &b) == EFS_ERR_PROTO);
    assert(!diff && !n && !a && !b);
    uint8_t got[4]; uint32_t gl = sizeof(got);
    assert(efs_kv_get(kv, key, sizeof(key), got, &gl) == EFS_ERR_NOT_FOUND);
    efs_kv_lsm_close(kv); free(image); efs_kv_mem_free(source);
    assert(!nftw(dir, cleanup, 32, FTW_DEPTH | FTW_PHYS));
    puts("pack snapshot: direct import, cold reopen, empty diff capability and unknown-reader rejection PASS");
}
