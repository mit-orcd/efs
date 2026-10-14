/* Live LSM candidate selection. No disk-format change; unknown bounds and
 * allocation refusal retain the full search. All calls hold l->mu. */
#ifndef EFS_KV_CANDIDATES_H
#define EFS_KV_CANDIDATES_H

#define KV_CANDIDATE_BYTES_MAX (4u * 1024u * 1024u)
struct kv_candidates {
    uint32_t first1, end1, next;
    int range, indexed;
};

static void kv_candidates_prepare(struct kv_lsm *l)
{
    uint64_t n = (uint64_t)l->n_l0 + l->n_l1;
    uint64_t words = (n + 63) / 64;
    if (l->candidate_valid || !words)
        return;
    if (words > KV_CANDIDATE_BYTES_MAX / (KV_RANGE_N * sizeof(uint64_t)))
        return;
    uint64_t *bits = realloc(l->candidate_bits, (size_t)words * KV_RANGE_N * sizeof(*bits));
    if (!bits)
        return;
    l->candidate_bits = bits;
    l->candidate_words = (uint32_t)words;
    memset(bits, 0, (size_t)words * KV_RANGE_N * sizeof(*bits));
    for (uint32_t i = 0; i < n; i++) {
        const struct seg_slot *s = i < l->n_l0 ? &l->l0[i] : &l->l1[i-l->n_l0];
        int lo = s->rlo, hi = s->rhi;
        if (lo < 0 || hi >= KV_RANGE_N || lo > hi) {
            lo = 0;
            hi = KV_RANGE_N - 1;
        }
        for (int r = lo; r <= hi; r++)
            bits[(size_t)r * words + i/64] |= UINT64_C(1) << (i%64);
    }
    l->candidate_valid = 1;
}

/* L1 is sorted and disjoint. First keys are immutable resident block-index
 * keys. Include the predecessor of seek; stop only after the whole prefix.
 * Any unavailable bound falls back to all L1, never a false absence. */
static void kv_candidates_init(struct kv_lsm *l, struct kv_candidates *c,
                              const uint8_t *seek, uint32_t slen,
                              const uint8_t *prefix, uint32_t plen,
                              int point, int private_read)
{
    int lo, hi;
    memset(c, 0, sizeof(*c));
    c->end1 = l->n_l1;
    kv_prefix_ranges(point ? seek : prefix, point ? slen : plen, &lo, &hi);
    c->range = lo;
    if (!private_read && lo == hi) {
        kv_candidates_prepare(l);
        c->indexed = l->candidate_valid;
    }
    if (private_read || !slen || !l->n_l1)
        return;
    uint32_t a = 0, b = l->n_l1;
    while (a < b) {
        uint32_t m = a + (b-a)/2, len;
        const uint8_t *key;
        if (kv_seg_first_key(l->l1[m].seg, &key, &len) != EFS_OK)
            return;
        if (kv_key_cmp(key, len, seek, slen) <= 0) a = m+1;
        else b = m;
    }
    uint32_t first = a ? a-1 : 0;
    uint32_t end = first+1;
    if (!point) {
        a = first; b = l->n_l1;
        while (a < b) {
            uint32_t m = a + (b-a)/2, len;
            const uint8_t *key;
            if (kv_seg_first_key(l->l1[m].seg, &key, &len) != EFS_OK)
                return;
            if (plen && !kv_has_prefix(key,len,prefix,plen) &&
                kv_key_cmp(key,len,prefix,plen) > 0) b = m;
            else a = m+1;
        }
        end = a;
    }
    c->first1 = first;
    c->end1 = end;
}

/* Return global segment-array position, preserving precedence exactly. No
 * bitmap pointer survives a lock drop; view revision checks precede reuse. */
static uint32_t kv_candidates_next(struct kv_lsm *l, struct kv_candidates *c)
{
    uint32_t n = l->n_l0 + l->n_l1;
    uint32_t at = c->next;
    for (;;) {
        if (at >= l->n_l0 && at < l->n_l0+c->first1)
            at = l->n_l0+c->first1;
        if (at >= l->n_l0+c->end1)
            return n;
        if (!c->indexed) {
            c->next = at+1;
            return at;
        }
        uint32_t w = at/64;
        uint64_t bits = l->candidate_bits[(size_t)c->range*l->candidate_words+w] &
                        (UINT64_MAX << (at%64));
        if (!bits) {
            at = (w+1)*64;
            continue;
        }
        uint32_t found = w*64+(uint32_t)__builtin_ctzll(bits);
        if (found >= l->n_l0 && found < l->n_l0+c->first1) {
            at = l->n_l0+c->first1;
            continue;
        }
        if (found >= l->n_l0+c->end1)
            return n;
        c->next = found+1;
        return found;
    }
}
#endif
