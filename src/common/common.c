#include "efs/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

int efs_chunk_size_valid(uint32_t chunk_size)
{
    if (chunk_size < EFS_MIN_CHUNK_SIZE || chunk_size > EFS_MAX_CHUNK_SIZE)
        return 0;
    return (chunk_size & (chunk_size - 1)) == 0;
}

void efs_ino_path_segments(efs_ino_t ino, char seg[EFS_INO_PATH_SEGS][5])
{
    /* Inline digits: 5×snprintf per call was measurable under PUT storms. */
    uint64_t v = (uint64_t)ino;
    for (int i = 0; i < EFS_INO_PATH_SEGS; i++) {
        uint64_t g = v % 10000ULL;
        v /= 10000ULL;
        seg[i][0] = (char)('0' + (g / 1000) % 10);
        seg[i][1] = (char)('0' + (g / 100) % 10);
        seg[i][2] = (char)('0' + (g / 10) % 10);
        seg[i][3] = (char)('0' + g % 10);
        seg[i][4] = '\0';
    }
}

const char *efs_strerror(int rc)
{
    switch (rc) {
    case EFS_OK:         return "success";
    case EFS_ERR_IO:     return "I/O error";
    case EFS_ERR_NOMEM:  return "out of memory";
    case EFS_ERR_NOT_FOUND: return "not found";
    case EFS_ERR_EXIST:  return "already exists";
    case EFS_ERR_INVAL:  return "invalid argument";
    case EFS_ERR_NET:    return "network error";
    case EFS_ERR_PROTO:  return "protocol error";
    case EFS_ERR_NO_QUORUM: return "no quorum";
    case EFS_ERR_DECODE: return "decode error";
    case EFS_ERR_CHECKSUM: return "checksum mismatch";
    case EFS_ERR_QUOTA:  return "quota exceeded";
    case EFS_ERR_NOT_EMPTY: return "directory not empty";
    case EFS_ERR_BUSY:   return "resource busy";
    case EFS_ERR_STALE:  return "stale generation";
    case EFS_ERR_ACCES:  return "permission denied";
    case EFS_ERR_NAMETOOLONG: return "name too long";
    case EFS_ERR_AGAIN:  return "try again";
    case EFS_ERR_OVERFLOW: return "query limit exceeded";
    case EFS_ERR_ILSEQ: return "unrepresentable name";
    case EFS_ERR_NODATA: return "attribute not found";
    case EFS_ERR_UNSUPPORTED: return "unsupported log encoding";
    case EFS_ERR_FRAG_FORMAT: return "unknown fragment format";
    case EFS_ERR_FRAG_IDENTITY: return "fragment identity mismatch";
    case EFS_ERR_FENCED: return "session fenced";
    case EFS_ERR_UNREGISTERED: return "session not registered on shard";
    case EFS_ERR_UNFORMED: return "cluster not formed (run efs-mgmt raft-mkfs)";
    default:             return "unknown error";
    }
}

uint64_t efs_parse_quota(const char *str)
{
    if (!str || !*str)
        return 0;

    char *end = NULL;
    double val = strtod(str, &end);
    if (val < 0 || end == str)
        return 0;

    uint64_t mult = 1;
    while (*end && isspace((unsigned char)*end))
        end++;

    if (*end) {
        switch (tolower((unsigned char)*end)) {
        case 'k': mult = 1024ULL; break;
        case 'm': mult = 1024ULL * 1024; break;
        case 'g': mult = 1024ULL * 1024 * 1024; break;
        case 't': mult = 1024ULL * 1024 * 1024 * 1024; break;
        default:  return 0;
        }
    }

    return (uint64_t)(val * (double)mult);
}

extern char **environ;

unsigned efs_faults_env_ignored(const char *prog)
{
    unsigned n = 0;

    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, "EFS_FAULT_", 10))
            continue;
        const char *eq = strchr(*e, '=');
        int len = eq ? (int)(eq - *e) : (int)strlen(*e);
        fprintf(stderr, "%s: %.*s ignored: this build has no fault hooks "
                "(build with EFS_FAULTS=1 for fault tests)\n", prog, len, *e);
        n++;
    }
    return n;
}

/* A6/A10: settings removed from the product. A launch that still sets one
 * (even empty) stops naming it, never runs without it. The inventory
 * (docs/status/a6-settings-inventory-20261024.md) lists every row; the
 * make test check (scripts/env_inventory.py) reads this table. */
static const struct { const char *name, *why; } removed_env[] = {
    { "EFS_MD_RAFT_N", "W153): the member count and the metadata voters come from the "
      "membership committed at formation (efs-mgmt raft-mkfs" },
    { "EFS_KV_COMPACT_DIE", "K24): the compaction crash hook is EFS_FAULT_COMPACT_DIE, "
      "in EFS_FAULTS builds only" },
    { "EFS_RDMA_SPIN_US", "dead-code audit, Oct 21): the RDMA receive path does not spin" },
    { "EFS_GC_DISABLE", "A6): test hook; EFS_FAULT_GC_DISABLE in EFS_FAULTS builds" },
    { "EFS_TEST_MEMBER_EXIT", "A6): test hook; EFS_FAULT_MEMBER_EXIT in EFS_FAULTS builds" },
    { "EFS_TEST_MEMBER_EXIT_ONCE", "A6): test hook; EFS_FAULT_MEMBER_EXIT_ONCE in EFS_FAULTS builds" },
    { "EFS_PACK_CRASH", "A6): test hook; EFS_FAULT_PACK_CRASH in EFS_FAULTS builds" },
    { "EFS_PACK_CRASH_NODE", "A6): test hook; EFS_FAULT_PACK_CRASH_NODE in EFS_FAULTS builds" },
    { "EFS_FEATURES_TTL_MS", "A6): the features cache keeps 2000 ms" },
    { "EFS_META_BATCH_OPS", "A6): FUSE metadata batches hold 4096 operations" },
    { "EFS_PACK_COMPACT_COLD_S", "A6): segments are cold after 86400 s" },
    { "EFS_PACK_COMPACT_DAILY_MIN", "A6): the daily compaction window is 120 min" },
    { "EFS_PACK_COMPACT_IDLE_MS", "A6): an idle compaction batch runs at most 10000 ms" },
    { "EFS_PUT_ORPHAN_PER_PASS", "A6): the orphan sweep takes 32 tickets per pass" },
    { "EFS_PUT_SYNC_BYTES", "A6): the PUT sync window is 256 MiB" },
    { "EFS_QUERY_CATALOG_MIB", "A6): the in-memory query catalog budget is 256 MiB" },
    { "EFS_RDMA_RNR_RETRY", "A6): the RNR retry count is 7 (retry forever)" },
    { "EFS_READ_FRAG_TIMEOUT_MS", "A6): a fragment attempt waits at most 2000 ms" },
    { "EFS_REPORT_RECEIPTS", "A6): the REPORT receipt table holds 4096 entries" },
};

unsigned efs_env_removed_refuse(const char *prog)
{
    unsigned n = 0;

    for (size_t i = 0; i < sizeof(removed_env) / sizeof(removed_env[0]); i++) {
        if (!getenv(removed_env[i].name))
            continue;
        fprintf(stderr, "%s: %s was removed (%s). Unset it and start again.\n",
                prog, removed_env[i].name, removed_env[i].why);
        n++;
    }
    return n;
}
