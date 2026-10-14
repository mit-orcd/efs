#ifndef EFS_READ_PEER_ORDER_H
#define EFS_READ_PEER_ORDER_H
#include "efs/common.h"

/* Read routing advice only: no node-health/quorum or placement change.
 * A failed preferred GET remains a last-choice candidate until that read's
 * existing deadline. Other verified fragments can satisfy the read; if they
 * fail too, the deferred peer is still attempted. Never refresh an active
 * hint on every chunk, or a recovered peer would never be tried again. */
struct efs_read_peer_order {
    uint64_t until[EFS_MAX_NODES];
};
static inline void efs_read_peer_defer(struct efs_read_peer_order *p,
                                      efs_node_id_t node, uint64_t now,
                                      uint64_t deadline)
{
    if (!node || node > EFS_MAX_NODES || deadline <= now) return;
    uint64_t old = __atomic_load_n(&p->until[node - 1], __ATOMIC_RELAXED);
    while (old <= now && !__atomic_compare_exchange_n(&p->until[node - 1],
            &old, deadline, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
static inline void efs_read_peer_succeeded(struct efs_read_peer_order *p,
                                          efs_node_id_t node)
{
    if (node && node <= EFS_MAX_NODES)
        __atomic_store_n(&p->until[node - 1], 0, __ATOMIC_RELAXED);
}
static inline void efs_read_peer_prefer(struct efs_read_peer_order *p,
                                        const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                        int order[EFS_NUM_FRAGMENTS], uint64_t now)
{
    int deferred[EFS_NUM_FRAGMENTS];
    for (unsigned i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        efs_node_id_t node = nodes[order[i]];
        deferred[i] = node && node <= EFS_MAX_NODES &&
            __atomic_load_n(&p->until[node - 1], __ATOMIC_RELAXED) > now;
    }
    /* Stable partition preserves local/data-first priority among equals. */
    for (unsigned i = 1; i < EFS_NUM_FRAGMENTS; i++)
        for (unsigned j = i; j && deferred[j - 1] > deferred[j]; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
            t = deferred[j]; deferred[j] = deferred[j - 1]; deferred[j - 1] = t;
        }
}
#endif
