#include "efs/placement.h"
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <netdb.h>

static uint64_t fnv1a_u64(uint64_t key)
{
    uint64_t hash = 14695981039346656037ULL;
    for (int i = 0; i < 8; i++) {
        hash ^= (key >> (i * 8)) & 0xFFULL;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Original ring: consecutive ranks from hash(ino, chunk). Meta pages are
 * located this way and must not move. */
static void place_consecutive(uint32_t node_count, efs_ino_t ino,
                              uint32_t chunk_index,
                              efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS])
{
    uint64_t key = ((uint64_t)ino << 32) | (uint64_t)chunk_index;
    uint32_t start = (uint32_t)(fnv1a_u64(key) % (uint64_t)node_count);
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
        fragment_nodes[i] = (efs_node_id_t)((start + i) % node_count + 1);
}

/* Widest spread: 3 distinct ranks, stride around the ring, start walks
 * with chunk_index so successive chunks of one file use different servers. */
static void place_wide(uint32_t node_count, efs_ino_t ino, uint32_t chunk_index,
                       efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS])
{
    uint32_t start = (uint32_t)((fnv1a_u64((uint64_t)ino) +
                                 (uint64_t)chunk_index) %
                                (uint64_t)node_count);
    uint32_t stride = node_count / (uint32_t)EFS_NUM_FRAGMENTS;
    if (stride < 1)
        stride = 1;

    uint8_t used[EFS_MAX_NODES];
    memset(used, 0, sizeof(used));
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        uint32_t idx = (start + (uint32_t)i * stride) % node_count;
        uint32_t hops = 0;
        while (used[idx] && hops < node_count) {
            idx = (idx + 1) % node_count;
            hops++;
        }
        used[idx] = 1;
        fragment_nodes[i] = (efs_node_id_t)(idx + 1);
    }
}

void efs_get_placement(uint32_t node_count, efs_ino_t ino, uint32_t chunk_index,
                       efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS])
{
    if (node_count == 0)
        node_count = 1;
    if (node_count > EFS_MAX_NODES)
        node_count = EFS_MAX_NODES;

    if (efs_ino_is_meta_table(ino))
        place_consecutive(node_count, ino, chunk_index, fragment_nodes);
    else
        place_wide(node_count, ino, chunk_index, fragment_nodes);
}

void efs_place_fragments(const struct efs_node *nodes, uint32_t node_count,
                         efs_ino_t ino, uint32_t chunk_index,
                         efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS])
{
    efs_node_id_t ranks[EFS_NUM_FRAGMENTS];
    efs_node_id_t ids[EFS_MAX_NODES];

    if (!nodes || node_count == 0) {
        efs_get_placement(1, ino, chunk_index, fragment_nodes);
        return;
    }
    if (node_count > EFS_MAX_NODES)
        node_count = EFS_MAX_NODES;

    for (uint32_t i = 0; i < node_count; i++)
        ids[i] = nodes[i].id;

    /* Stable ring: sort member ids ascending. */
    for (uint32_t i = 1; i < node_count; i++) {
        efs_node_id_t key = ids[i];
        uint32_t j = i;
        while (j > 0 && ids[j - 1] > key) {
            ids[j] = ids[j - 1];
            j--;
        }
        ids[j] = key;
    }

    efs_get_placement(node_count, ino, chunk_index, ranks);
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        uint32_t rank = (uint32_t)ranks[i]; /* 1-based */
        if (rank == 0 || rank > node_count)
            fragment_nodes[i] = ids[0];
        else
            fragment_nodes[i] = ids[rank - 1];
    }
}

static bool host_match(const char *a, const char *b)
{
    size_t i = 0;
    while (a[i] && b[i]) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

/* Σ min(caps[i], m) ≥ 3 m ?  Saturating: 3 m and the sum can exceed
 * 2^64 only with absurd inputs; clamp so the comparison stays true. */
static int cap_fits(const uint64_t *caps, uint32_t n, uint64_t m)
{
    uint64_t sum = 0, need;
    uint32_t i;

    if (m > UINT64_MAX / 3)
        return 0;
    need = 3 * m;
    for (i = 0; i < n; i++) {
        uint64_t c = caps[i] < m ? caps[i] : m;
        if (sum > UINT64_MAX - c)
            return 1;
        sum += c;
    }
    return sum >= need;
}

uint64_t efs_capacity_logical(const uint64_t *caps, uint32_t n)
{
    uint64_t lo = 0, hi = 0;
    uint32_t k, nonzero = 0;

    if (!caps || n < EFS_NUM_FRAGMENTS)
        return 0;
    for (k = 0; k < n; k++) {
        if (caps[k] == 0)
            continue;
        nonzero++;
        /* M never exceeds the sum / 3; the sum is a safe upper bound. */
        hi = hi > UINT64_MAX - caps[k] ? UINT64_MAX : hi + caps[k];
    }
    if (nonzero < EFS_NUM_FRAGMENTS)
        return 0;
    /* cap_fits is monotone decreasing in m: binary search the largest m. */
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo + 1) / 2;
        if (cap_fits(caps, n, mid))
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo > UINT64_MAX / 2 ? UINT64_MAX : lo * 2;
}

bool efs_is_local_node(const struct efs_node *nodes, uint32_t node_count,
                       const char *host, efs_node_id_t *local_id)
{
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0)
        hostname[0] = '\0';
    hostname[sizeof(hostname) - 1] = '\0';

    for (uint32_t i = 0; i < node_count; i++) {
        if (host_match(nodes[i].addr, host) ||
            host_match(nodes[i].addr, hostname) ||
            host_match(nodes[i].addr, "localhost") ||
            host_match(nodes[i].addr, "127.0.0.1")) {
            if (local_id)
                *local_id = nodes[i].id;
            return true;
        }
    }
    (void)hostname;
    return false;
}
