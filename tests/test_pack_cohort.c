#include "efs/pack_cohort.h"
#include "efs/checksum.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(void)
{
    struct efs_pack_cohort *c = malloc(sizeof(*c)), *saved = malloc(sizeof(*saved)); assert(c && saved);
    const uint64_t delay = 1000000;
    assert(!efs_pack_cohort_init(c, 0, 16, delay));
    assert(!efs_pack_cohort_due(c, UINT64_MAX, 1));
    uint8_t payload[4096]; memset(payload, 71, sizeof(payload));
    struct efs_pack_record record = {101, 3, 0, 7, 512, payload};
    assert(!efs_pack_cohort_offer(c, &record, 1, 100));
    assert(!efs_pack_cohort_due(c, 100 + delay - 1, 0));
    assert(efs_pack_cohort_due(c, 100 + delay, 0));
    assert(efs_pack_cohort_due(c, 99, 0) && efs_pack_cohort_due(c, 100, 1));
    *saved = *c; assert(efs_pack_cohort_offer(c, &record, 2, 100) == EFS_ERR_BUSY);
    assert(!memcmp(saved, c, sizeof(*c)));
    for (unsigned i = 1; i < 16; i++) { record.ino = 101 + 2*i; record.record_generation++;
        assert(!efs_pack_cohort_offer(c, &record, i + 1, 100)); }
    assert(efs_pack_cohort_due(c, 100, 0));
    memset(payload, 99, sizeof(payload));
    struct efs_opid owner = {.client_uuid = {19}, .session_epoch = 3, .seq = 1};
    struct efs_opid invalid_owner = owner; invalid_owner.session_epoch = 0;
    *saved = *c;
    assert(efs_pack_cohort_seal(c, &invalid_owner) == EFS_ERR_INVAL && !memcmp(saved,c,sizeof(*c)));
    assert(!efs_pack_cohort_seal(c, &owner));
    assert(c->count == 16 && c->geometry == 16384);
    uint8_t image_hash[32], retry_hash[32]; efs_hash(c->image, c->geometry, image_hash);
    assert(!efs_pack_cohort_seal(c, &owner));
    efs_hash(c->image, c->geometry, retry_hash); assert(!memcmp(image_hash, retry_hash, 32));
    owner.seq++; assert(efs_pack_cohort_seal(c, &owner) == EFS_ERR_PROTO);
    *saved = *c; record.ino += 2;
    assert(efs_pack_cohort_offer(c, &record, 17, 100) == EFS_ERR_BUSY && !memcmp(saved,c,sizeof(*c)));
    for (unsigned i = 0; i < c->count; i++) {
        const uint8_t *data; uint32_t len;
        assert(c->snapshots[i] == i + 1);
        assert(!efs_pack_read(c->image, c->geometry, c->refs + i, &data, &len) && len == 512);
        for (unsigned j = 0; j < len; j++) assert(data[j] == 71);
    }
    /* A blocked close/fsync can flush one record without waiting for another
     * producer. The largest payloads stop at actual geometry capacity. */
    assert(!efs_pack_cohort_init(c, 0, 128, delay));
    record.ino = 101; record.len = 1;
    assert(!efs_pack_cohort_offer(c, &record, 1, 0));
    assert(efs_pack_cohort_due(c, 0, 1)); assert(!efs_pack_cohort_seal(c, &owner));
    assert(c->geometry == EFS_SEGMENT_ALIGNMENT);
    assert(!efs_pack_cohort_init(c, 0, 128, delay)); record.len = sizeof(payload);
    unsigned count = 0;
    while (!efs_pack_cohort_offer(c, &record, count + 1, 0)) { count++; record.ino += 2; }
    assert(count == 31 && c->count == 31 && efs_pack_cohort_due(c, 0, 0));
    assert(!efs_pack_cohort_seal(c, &owner) && c->geometry == EFS_SEGMENT_MAX_SIZE);
    /* First-offer validation cannot mutate an empty queue. */
    assert(!efs_pack_cohort_init(c, 0, 16, delay)); *saved = *c;
    record.ino = 102; record.len = 512;
    assert(efs_raft_shard_group(efs_kv_lane_shard(record.ino, 0)) != c->group);
    assert(efs_pack_cohort_offer(c, &record, 1, 0) == EFS_ERR_INVAL && !memcmp(saved,c,sizeof(*c)));
    record.ino = 101;
    record.len = 0; assert(efs_pack_cohort_offer(c, &record, 1, 0) == EFS_ERR_INVAL);
    assert(!memcmp(saved,c,sizeof(*c)));
    free(saved); free(c);
    puts("pack cohort: copied snapshots, one-group bound, deadline/sync/singleton flush, capacity and immutable retry PASS");
    return 0;
}
