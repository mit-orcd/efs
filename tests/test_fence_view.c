#include "efs/fence_view.h"
#include <assert.h>
#include <stdio.h>

#define CS 1024u
struct objects {
    uint8_t bytes[EFS_FENCE_PART_MAX][CS];
    unsigned calls;
    int fail;
};

static int load(void *arg, uint32_t part, uint32_t off, uint32_t len,
                uint8_t *dst)
{
    struct objects *o = arg;
    ++o->calls;
    if (o->fail)
        return EFS_ERR_IO;
    memcpy(dst, o->bytes[part] + off, len);
    return EFS_OK;
}

static void example(void)
{
    struct efs_fence_history h = {0};
    struct efs_fence_part p[] = {{0, 0, CS}, {1, 500, 100}};
    struct efs_fence_view v;
    struct objects o = {0};
    uint8_t out[CS];
    memset(o.bytes[0], 'a', CS);
    memset(o.bytes[1], 'b', CS);
    assert(efs_fence_history_append(&h, 1, 100) == EFS_OK);
    assert(efs_fence_history_append(&h, 2, 700) == EFS_OK);
    assert(efs_fence_view_build(&v, 2, 0, CS, &h, p, 2) == EFS_OK);
    /* A read owns the resolved masks even after history retirement. */
    memset(&h, 0, sizeof(h));
    assert(efs_fence_materialize(&v, out, CS, load, &o) == EFS_OK);
    for (unsigned i = 0; i < CS; ++i)
        assert(out[i] == (i < 100 ? 'a' : i >= 500 && i < 600 ? 'b' : 0));
    assert(o.calls == 2);
    p[0].off = 0; p[0].len = CS;
    assert(efs_fence_history_append(&h, 3, CS) == EFS_OK);
    assert(efs_fence_view_build(&v, 3, CS, CS, &h, p, 1) == EFS_OK);
    o.calls = 0;
    assert(efs_fence_materialize(&v, out, CS, load, &o) == EFS_OK);
    assert(o.calls == 0); /* dead object: no GET */
    for (unsigned i = 0; i < CS; ++i) assert(!out[i]);
}

static void validation(void)
{
    struct efs_fence_history h = {0}, saved;
    struct efs_fence_view v, before;
    struct efs_fence_part p = {0, 0, CS};
    struct objects o = {0};
    uint8_t out[CS];
    for (unsigned i = 1; i <= EFS_FENCE_HISTORY_MAX; ++i)
        assert(efs_fence_history_append(&h, i, CS - i) == EFS_OK);
    saved = h;
    assert(efs_fence_history_append(&h, EFS_FENCE_HISTORY_MAX,
                                   CS - EFS_FENCE_HISTORY_MAX) == EFS_OK);
    assert(efs_fence_history_append(&h, EFS_FENCE_HISTORY_MAX + 1, 0) == EFS_ERR_BUSY);
    assert(!memcmp(&h, &saved, sizeof(h)));
    assert(efs_fence_history_append(&h, 1, 0) == EFS_ERR_INVAL);
    assert(efs_fence_view_build(&v, EFS_FENCE_HISTORY_MAX, 0, CS, &h, &p, 1) == EFS_OK);
    before = v;
    p.len = CS + 1;
    assert(efs_fence_view_build(&v, EFS_FENCE_HISTORY_MAX, 0, CS, &h, &p, 1) == EFS_ERR_INVAL);
    assert(!memcmp(&v, &before, sizeof(v)));
    p.len = CS;
    assert(efs_fence_view_build(&v, EFS_FENCE_HISTORY_MAX, UINT64_MAX - CS + 1,
                              CS, &h, &p, 1) == EFS_ERR_INVAL);
    assert(efs_fence_view_build(&v, 0, 0, CS, &h, &p, 1) == EFS_ERR_INVAL);
    o.fail = 1;
    assert(efs_fence_materialize(&v, out, CS, load, &o) == EFS_ERR_IO);
    assert(efs_fence_materialize(&v, out, CS - 1, load, &o) == EFS_ERR_INVAL);
}

static void overlapping_parts(void)
{
    struct efs_fence_history h = {0};
    struct efs_fence_part p[] = {
        {2, 0, CS}, {0, 100, 300}, {1, 200, 300}, {2, CS, 0}
    };
    struct efs_fence_view v;
    struct objects o = {0};
    uint8_t out[CS];
    /* Absolute fence sizes on a nonzero chunk; the old span is clipped
     * inside its interval, and its missing suffix preserves the new base. */
    assert(efs_fence_history_append(&h, 1, CS + 250) == EFS_OK);
    assert(efs_fence_history_append(&h, 2, CS + 350) == EFS_OK);
    memset(o.bytes[0], 'a', CS);
    memset(o.bytes[1], 'b', CS);
    memset(o.bytes[2], 'c', CS);
    assert(efs_fence_view_build(&v, 2, CS, CS, &h, p, 4) == EFS_OK);
    assert(efs_fence_materialize(&v, out, CS, load, &o) == EFS_OK);
    assert(o.calls == 3); /* no GET for a fold tombstone */
    for (unsigned i = 0; i < CS; ++i)
        assert(out[i] == (i >= 200 && i < 350 ? 'c' :
                         i >= 100 && i < 200 ? 'b' : 'a'));
}

static uint32_t rng_state = 0x25;
static uint32_t random_word(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    return rng_state ^= rng_state << 5;
}

/* Independent byte-array oracle: shrink zeros discarded bytes immediately;
 * extension never changes them. Compare with deferred per-part fencing after
 * every operation, folding through the same production helper when full. */
static void randomized(void)
{
    struct efs_fence_history h = {0};
    struct efs_fence_part p[EFS_FENCE_PART_MAX] = {{0, 0, CS}};
    struct objects o = {0};
    uint8_t model[CS], out[CS];
    uint64_t epoch = 0;
    uint32_t count = 1, size = CS;
    memset(model, 37, CS);
    memcpy(o.bytes[0], model, CS);
    for (unsigned step = 0; step < 20000; ++step) {
        struct efs_fence_view v;
        assert(efs_fence_view_build(&v, epoch, 0, CS, &h, p, count) == EFS_OK);
        assert(efs_fence_materialize(&v, out, CS, load, &o) == EFS_OK);
        assert(!memcmp(out, model, CS));
        if (count == EFS_FENCE_PART_MAX || h.count == EFS_FENCE_HISTORY_MAX) {
            /* Simulated successful exact-version sweep/fold, no races here. */
            memcpy(o.bytes[0], out, CS);
            p[0] = (struct efs_fence_part){epoch, 0, CS};
            count = 1;
            memset(&h, 0, sizeof(h));
        }
        uint32_t op = random_word() % 3;
        if (op == 0) {
            uint32_t next = random_word() % (CS + 1);
            if (next < size) {
                assert(efs_fence_history_append(&h, ++epoch, next) == EFS_OK);
                memset(model + next, 0, CS - next);
            }
            size = next;
        } else if (op == 1) {
            size = CS; /* extend without a fence */
        } else {
            uint32_t off = random_word() % CS;
            uint32_t len = 1 + random_word() % (CS - off);
            uint8_t value = (uint8_t)(1 + random_word() % 255);
            p[count] = (struct efs_fence_part){epoch, off, len};
            memset(o.bytes[count], value, CS);
            memset(model + off, value, len);
            ++count;
            if (off + len > size) size = off + len;
        }
    }
}

int main(void)
{
    example();
    validation();
    overlapping_parts();
    randomized();
    puts("fence view: example, history bound, validation, 20000 model operations PASS");
    return 0;
}
