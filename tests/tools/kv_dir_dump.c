/* kv_dir_dump — print one inode's row(s) and every dentry under it, from a
 * COPY of an LSM directory (never the live dir: gc_orphans would delete a
 * segment the daemon is writing). For "rmdir: Directory not empty" on a
 * directory `ls` shows empty, or a `d?????????` name `stat` cannot reach:
 *
 *   ino=$(stat -c %i /tmp/efs-mount/path/to/dir)
 *   cp -r /data1/01/efs/mdraft/kv /tmp/kvcopy      # on the node
 *   make tests/tools/kv_dir_dump                    # on a node
 *   ./tests/tools/kv_dir_dump /tmp/kvcopy $ino [child-ino ...]
 *
 * Prints: every INODE row whose ino matches (there must be exactly one;
 * two shards = duplicate ino), its nlink / nents / layout / used_shards,
 * every DENTRY whose parent is the ino (name, child ino, type, and whether
 * the child's INODE row exists = dangling if not), and any pending
 * INTENT / GUARD / REDUCE wrapped around those keys. Extra inos are
 * dumped the same way (pass the children `ls` does show, to compare).
 * One node's KV holds only its groups' shards; run on a node of each group
 * (fcstor004/005 host both) or accept "no row" for the other parity. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"

struct want {
    efs_ino_t ino;
    uint8_t ikey[EFS_KV_KEY_MAX]; /* efs_kv_key_inode(0, ino) */
    uint32_t ilen;
    uint8_t dpre[EFS_KV_KEY_MAX]; /* efs_kv_key_dentry_prefix(0, ino) */
    uint32_t dlen;
    unsigned rows, dents;
};

struct acc {
    struct efs_kv *kv;
    struct want *w;
    int n;
};

static int child_row_exists(struct efs_kv *kv, efs_ino_t ino)
{
    /* The child's row is on ino's own shard; we do not know the hash here,
     * so probe every shard (4096 gets on a copy is fine). */
    uint8_t key[EFS_KV_KEY_MAX], val[4096];
    uint32_t klen, vlen, s;

    for (s = 0; s <= EFS_KV_SHARD_MASK; s++) {
        if (efs_kv_key_inode(s, ino, key, &klen) != EFS_OK)
            return -1;
        vlen = sizeof(val);
        if (efs_kv_get(kv, key, klen, val, &vlen) == EFS_OK)
            return 1;
    }
    return 0;
}

static void print_row(const char *tag, uint32_t shard, const uint8_t *val,
                      uint32_t vlen)
{
    struct efs_meta_row r;

    if (efs_meta_unpack_inode(val, vlen, &r) != EFS_OK) {
        printf("%s shard=%u: unpack failed (vlen=%u)\n", tag, shard, vlen);
        return;
    }
    printf("%s shard=%-4u ino=%" PRIu64 " gen=%" PRIu64 " mode=%o nlink=%u "
           "nents=%u layout=%u used_shards=%#" PRIx64 " parent=%" PRIu64
           " size=%" PRIu64 " pver=%" PRIu64 "\n",
           tag, shard, (uint64_t)r.ino, r.generation, r.mode, r.nlink, r.nents,
           r.layout, r.used_shards, (uint64_t)r.parent, r.base_size,
           r.parent_version);
}

static int cb(void *user, const uint8_t *key, uint32_t klen, const uint8_t *val,
              uint32_t vlen)
{
    struct acc *a = user;
    int i, kind, wrapped = 0;
    const uint8_t *k = key;
    uint32_t kl = klen, shard;

    if (klen < 3)
        return 0;
    shard = ((uint32_t)key[0] << 8) | key[1];
    kind = key[2];
    if (kind == EFS_KV_KIND_INTENT || kind == EFS_KV_KIND_GUARD ||
        kind == EFS_KV_KIND_REDUCE) {
        /* wrap(): [shard][kind][orig kind][orig rest][txid for GUARD/REDUCE] */
        wrapped = kind;
        k = key + 1; /* now k[2] is the original kind, k[3..] the original rest */
        kl = klen - 1;
        if (kind != EFS_KV_KIND_INTENT && kl >= 16)
            kl -= 16;
        kind = k[2];
    }
    for (i = 0; i < a->n; i++) {
        struct want *w = &a->w[i];
        char tag[32];

        if (wrapped)
            snprintf(tag, sizeof(tag), "  pending-%s on",
                     wrapped == EFS_KV_KIND_INTENT ? "INTENT"
                     : wrapped == EFS_KV_KIND_GUARD ? "GUARD" : "REDUCE");
        else
            snprintf(tag, sizeof(tag), "%s", "");
        if (kind == EFS_KV_KIND_INODE && kl == w->ilen &&
            memcmp(k + 3, w->ikey + 3, kl - 3) == 0) {
            if (wrapped)
                printf("%s INODE ino=%" PRIu64 " shard=%u vlen=%u\n", tag,
                       (uint64_t)w->ino, shard, vlen);
            else {
                w->rows++;
                print_row("INODE", shard, val, vlen);
            }
        } else if (kind == EFS_KV_KIND_DENTRY && kl > w->dlen &&
                   memcmp(k + 3, w->dpre + 3, w->dlen - 3) == 0) {
            const char *name = (const char *)k + w->dlen;
            uint32_t nlen = kl - w->dlen;
            struct efs_meta_dentry d;
            int ok = !wrapped && efs_meta_unpack_dentry(val, vlen, &d) == EFS_OK;

            if (!wrapped)
                w->dents++;
            printf("%s DENTRY parent=%" PRIu64 " shard=%-4u name=\"%.*s\"",
                   wrapped ? tag : "", (uint64_t)w->ino, shard, (int)nlen, name);
            if (ok) {
                int ex = child_row_exists(a->kv, d.ino);
                printf(" -> ino=%" PRIu64 " gen=%" PRIu64 " type=%u child-row=%s",
                       (uint64_t)d.ino, d.generation, d.type,
                       ex > 0 ? "present" : ex == 0 ? "MISSING (dangling)" : "?");
            } else if (!wrapped)
                printf(" (unpack failed vlen=%u)", vlen);
            printf("\n");
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct efs_kv_lsm_cfg cfg;
    struct efs_kv *kv;
    struct acc a;
    int i, rc;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <kv-dir-copy> <ino> [ino ...]\n", argv[0]);
        return 2;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = EFS_KV_LSM_NOSYNC;
    kv = efs_kv_lsm_open(argv[1], &cfg);
    if (!kv) {
        fprintf(stderr, "open %s failed\n", argv[1]);
        return 1;
    }
    a.kv = kv;
    a.n = argc - 2;
    a.w = calloc((size_t)a.n, sizeof(*a.w));
    for (i = 0; i < a.n; i++) {
        struct want *w = &a.w[i];
        w->ino = (efs_ino_t)strtoull(argv[2 + i], NULL, 0);
        if (efs_kv_key_inode(0, w->ino, w->ikey, &w->ilen) != EFS_OK ||
            efs_kv_key_dentry_prefix(0, w->ino, w->dpre, &w->dlen) != EFS_OK) {
            fprintf(stderr, "bad ino %s\n", argv[2 + i]);
            return 2;
        }
    }
    rc = efs_kv_scan(kv, cb, &a);
    printf("scan rc=%d\n", rc);
    for (i = 0; i < a.n; i++)
        printf("ino %" PRIu64 ": %u inode row(s) on this node, %u dentr%s\n",
               (uint64_t)a.w[i].ino, a.w[i].rows, a.w[i].dents,
               a.w[i].dents == 1 ? "y" : "ies");
    efs_kv_lsm_close(kv);
    free(a.w);
    return 0;
}
