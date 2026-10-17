#ifndef EFS_APPLY_RING_H
#define EFS_APPLY_RING_H
#include <stdint.h>
/* One apply-result ring slot: (index, term, rc, extra) of an applied entry,
 * plus the apply time when EFS_WRITE_PROF is on (else 0).
 *
 * Readers copy a slot without h->mu, so every field is accessed atomically;
 * a sequence counter alone around plain fields is still a C data race
 * (WG21 P1478). Protocol (Boehm, "Can seqlocks get along with programming
 * language memory models?", MSPC 2012):
 *   writer: seq odd (relaxed), release fence, relaxed field stores,
 *           seq even (release);
 *   reader: seq (acquire), relaxed field loads, acquire fence, seq again
 *           (relaxed); equal and even = coherent copy.
 * Writers of one slot must be serialized by the caller (h->mu: host_apply
 * runs under the pump's hold; the attach reset runs before readers). A
 * reader that loses the race `tries` times returns 0 and the caller falls
 * back to reading under the writers' lock. The 32-bit sequence would need
 * 2^31 rewrites of one slot inside a single read to alias. */
struct efs_arc_slot {
    uint32_t seq;
    int32_t rc;
    uint64_t idx, term, extra, apply_us;
};

struct efs_arc_view {
    uint64_t idx, term, extra, apply_us;
    int32_t rc;
};

static inline void efs_arc_store(struct efs_arc_slot *s, uint64_t idx,
                                 uint64_t term, int32_t rc, uint64_t extra,
                                 uint64_t apply_us)
{
    uint32_t q = __atomic_load_n(&s->seq, __ATOMIC_RELAXED);

    __atomic_store_n(&s->seq, q + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&s->idx, idx, __ATOMIC_RELAXED);
    __atomic_store_n(&s->term, term, __ATOMIC_RELAXED);
    __atomic_store_n(&s->rc, rc, __ATOMIC_RELAXED);
    __atomic_store_n(&s->extra, extra, __ATOMIC_RELAXED);
    __atomic_store_n(&s->apply_us, apply_us, __ATOMIC_RELAXED);
    __atomic_store_n(&s->seq, q + 2, __ATOMIC_RELEASE);
}

/* 1 = *v is one writer's complete record; 0 = still racing after `tries`. */
static inline int efs_arc_load(const struct efs_arc_slot *s,
                               struct efs_arc_view *v, unsigned tries)
{
    for (unsigned i = 0; i < tries; i++) {
        uint32_t a = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);

        if (a & 1)
            continue;
        v->idx = __atomic_load_n(&s->idx, __ATOMIC_RELAXED);
        v->term = __atomic_load_n(&s->term, __ATOMIC_RELAXED);
        v->rc = __atomic_load_n(&s->rc, __ATOMIC_RELAXED);
        v->extra = __atomic_load_n(&s->extra, __ATOMIC_RELAXED);
        v->apply_us = __atomic_load_n(&s->apply_us, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s->seq, __ATOMIC_RELAXED) == a)
            return 1;
    }
    return 0;
}
#endif
