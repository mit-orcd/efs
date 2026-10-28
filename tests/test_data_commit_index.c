/* An 8 GiB/128 KiB file must not walk all earlier chunks at every PUT or
 * again at fsync. Count inspected records, not wall time or CPU speed.
 * Reuse the real K35 implementation and its storage/proof stubs. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define NCHUNKS (1u << 16)
#define STRIPE_CAP (1u << 17)
static uint64_t inspected;
static void inspect_record(void)
{
    /* Old inode-only lookup exceeds this after about 2,000 chunks. */
    assert(++inspected <= 32ull * NCHUNKS);
}
#define EFS_COMMIT_LOOKUP_PROBE() inspect_record()
#define main existing_proof_tests
#include "test_data_commit.c"
#undef main

int main(void)
{
    for (int i = 1; i < NN; i++) epochs[i] = 101;
    const efs_ino_t ino = 700;
    for (uint32_t ci = 0; ci < NCHUNKS; ci++)
        assert(put(ino, ci, 1, 2, 3, 7) == EFS_OK);
    /* Same chunk and a different PUT object both survive. A resend updates
     * the exact object, never adds a duplicate or changes another version. */
    struct efs_put_ticket t = {.ino = ino, .inode_gen = 5, .ci = 3,
        .object = 0xabcdef0123456789ull, .nodes = {1, 2, 3}};
    /* Force an exact-index collision between versions of the same chunk. */
    while (identity_bucket(ino, 3, t.object) != identity_bucket(ino, 3, ino*1000+3))
        t.object++;
    uint64_t seq[3], ep[3] = {101, 101, 101};
    for (int i = 0; i < 3; i++) seq[i] = efs_client_commit_note(i+1);
    assert(efs_client_stripe_note(&t, seq, ep) == EFS_OK);
    for (int i = 0; i < 3; i++) seq[i] = efs_client_commit_note(i+1);
    assert(efs_client_stripe_note(&t, seq, ep) == EFS_OK);
    assert(live() == NCHUNKS+1);
    assert(stripe_find(ino, 3, t.object)->seq[0] == seq[0]);
    assert(stripe_find(ino, 3, ino*1000+3));
    /* A second inode in the same inode bucket must survive a target-only
     * proof, including arbitrary removal/reinsertion of its neighbours. */
    efs_ino_t peer = ino+1;
    while (bucket_of(peer) != bucket_of(ino)) peer++;
    assert(put(peer, 7, 1, 2, 3, 7) == EFS_OK);
    /* Remove in a permutation, including interior nodes, and reuse slots.
     * The per-inode scan and exact-object index must stay coherent. */
    pthread_mutex_lock(&commit_mu);
    for (uint32_t k = 0; k < NCHUNKS/2; k++) {
        uint32_t ci = (k*8191u) & (NCHUNKS-1);
        stripe_remove(ino, ci, ino*1000+ci);
        assert(!stripe_find(ino, ci, ino*1000+ci));
    }
    pthread_mutex_unlock(&commit_mu);
    assert(live() == NCHUNKS/2+2);
    for (uint32_t k = 0; k < NCHUNKS/2; k++) {
        uint32_t ci = (k*8191u) & (NCHUNKS-1);
        assert(put(ino, ci, 1, 2, 3, 7) == EFS_OK);
    }
    assert(live() == NCHUNKS+2);
    reset();
    assert(efs_client_data_commit(ino) == EFS_OK);
    assert(live() == 1 && !marks && commits[1] == 1 && commits[2] == 1 && commits[3] == 1);
    assert(stripe_find(peer, 7, peer*1000+7));
    assert(efs_client_data_commit(peer) == EFS_OK && !live());
    assert(inspected > NCHUNKS);
    printf("large-file durability index: 65,536 chunks, exact versions, resend, "
           "arbitrary removal, slot reuse and isolated fsync PASS; probes=%llu\n",
           (unsigned long long)inspected);
    return 0;
}
