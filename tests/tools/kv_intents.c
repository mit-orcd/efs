/* kv_intents — list the transaction records (INTENT / GUARD / REDUCE /
 * DECISION) left in an LSM directory, with their age. Run it on a COPY of
 * <storage>/mdraft/kv (opening the live dir would let gc_orphans delete a
 * segment the daemon is writing):
 *
 *   cp -r /data1/01/efs/mdraft/kv /tmp/kvcopy
 *   make tests/tools/kv_intents        # generic %: %.c $(LIB) rule, on a node
 *   ./tests/tools/kv_intents /tmp/kvcopy [min_age_s]
 *
 * A txid's first 8 bytes are the proposer's now_ns() (fill_txid), so the
 * age is exact to the clock skew between servers. Every record older than
 * a few seconds on an idle cluster is a stranded transaction: its DECIDE
 * or RESOLVE was never proposed (Sep 21: DECIDE wait BUSY at 400 ms →
 * the RESOLVE loop skipped → an EXCL intent on a shard's ALLOC key makes
 * every log-path create/mkdir on that shard BUSY forever). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"

struct acc {
    uint64_t now_ns;
    double min_age;
    unsigned n[32];
    unsigned old[32];
    unsigned alloc_intents;
    unsigned shards_alloc[4096];
};

static const char *kind_name(int k)
{
    switch (k) {
    case EFS_KV_KIND_ALLOC: return "ALLOC";
    case EFS_KV_KIND_INODE: return "INODE";
    case EFS_KV_KIND_DENTRY: return "DENTRY";
    case EFS_KV_KIND_CHUNK: return "CHUNK";
    case EFS_KV_KIND_LANE: return "LANE";
    case EFS_KV_KIND_VER: return "VER";
    case EFS_KV_KIND_INTENT: return "INTENT";
    case EFS_KV_KIND_GUARD: return "GUARD";
    case EFS_KV_KIND_REDUCE: return "REDUCE";
    case EFS_KV_KIND_DECISION: return "DECISION";
    case EFS_KV_KIND_DSEQ: return "DSEQ";
    default: return "?";
    }
}

static uint64_t txid_ns(const uint8_t *t)
{
    uint64_t v;
    memcpy(&v, t, 8); /* host order, as fill_txid wrote it */
    return v;
}

static int cb(void *user, const uint8_t *key, uint32_t klen, const uint8_t *val,
              uint32_t vlen)
{
    struct acc *a = user;
    int kind, okind = -1;
    uint32_t shard;
    const uint8_t *txid = NULL;
    double age;

    if (klen < 3)
        return 0;
    kind = key[2];
    shard = ((uint32_t)key[0] << 8) | key[1];
    switch (kind) {
    case EFS_KV_KIND_INTENT:
        /* value: txid[16] parts... ; key: [shard][8][orig kind][orig rest] */
        if (vlen >= 16)
            txid = val;
        okind = key[3]; /* wrap(): [shard][kind][orig kind][orig rest] */
        break;
    case EFS_KV_KIND_GUARD:
    case EFS_KV_KIND_REDUCE:
        if (klen >= 16 + 3)
            txid = key + klen - 16;
        okind = key[3]; /* wrap(): [shard][kind][orig kind][orig rest] */
        break;
    case EFS_KV_KIND_DECISION:
        if (klen >= 3 + 16)
            txid = key + 3;
        break;
    default:
        return 0;
    }
    if (kind < 32)
        a->n[kind]++;
    if (!txid)
        return 0;
    age = (double)(int64_t)(a->now_ns - txid_ns(txid)) / 1e9;
    if (age < a->min_age)
        return 0;
    if (kind < 32)
        a->old[kind]++;
    if (kind == EFS_KV_KIND_INTENT && okind == EFS_KV_KIND_ALLOC) {
        a->alloc_intents++;
        if (shard < 4096)
            a->shards_alloc[shard]++;
    }
    if (kind != EFS_KV_KIND_DECISION)
        printf("%-8s shard=%-4u on=%-7s age=%.1fs klen=%u vlen=%u\n",
               kind_name(kind), shard, okind >= 0 ? kind_name(okind) : "-",
               age, klen, vlen);
    return 0;
}

int main(int argc, char **argv)
{
    struct efs_kv_lsm_cfg cfg;
    struct efs_kv *kv;
    struct acc a;
    struct timespec ts;
    int i, rc;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <kv-dir-copy> [min_age_s]\n", argv[0]);
        return 2;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.sync_mode = EFS_KV_LSM_NOSYNC;
    kv = efs_kv_lsm_open(argv[1], &cfg);
    if (!kv) {
        fprintf(stderr, "open %s failed\n", argv[1]);
        return 1;
    }
    memset(&a, 0, sizeof(a));
    clock_gettime(CLOCK_REALTIME, &ts);
    a.now_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    a.min_age = argc > 2 ? atof(argv[2]) : 5.0;
    rc = efs_kv_scan(kv, cb, &a);
    printf("scan rc=%d\n", rc);
    for (i = 0; i < 32; i++)
        if (a.n[i])
            printf("total %-8s %u (older than %.0fs: %u)\n", kind_name(i), a.n[i],
                   a.min_age, a.old[i]);
    printf("stranded ALLOC intents: %u on", a.alloc_intents);
    for (i = 0; i < 4096; i++)
        if (a.shards_alloc[i])
            printf(" %d", i);
    printf("\n");
    efs_kv_lsm_close(kv);
    return 0;
}
