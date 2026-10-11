#define _XOPEN_SOURCE 700
#include "efs/pack_admission.h"
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include "efs/session.h"
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int cleanup(const char *p, const struct stat *s, int t, struct FTW *f)
{ (void)s; (void)t; (void)f; return remove(p); }
static void make(struct efs_pack_object *o, struct efs_pack_ref refs[2], uint64_t seq, uint32_t group)
{
    struct efs_opid owner = {.client_uuid = {11}, .session_epoch = 7, .seq = seq};
    owner.client_uuid[0] += group;
    uint64_t id, gen; assert(!efs_pack_identity(&owner, group, &id, &gen));
    uint8_t payload[512] = {17}, image[EFS_SEGMENT_MAX_SIZE]; size_t geometry;
    struct efs_pack_record records[] = {{101, 3, 0, 19, 512, payload}, {103, 3, 0, 23, 512, payload}};
    for (unsigned i = 0; i < 2; i++) records[i].ino += group ? 1 : 0;
    assert(!efs_pack_build(id, gen, group, records, 2, image, sizeof(image), &geometry, refs));
    memset(o, 0, sizeof(*o));
    assert(!efs_pack_ledger_init(image, geometry, refs, 2, &owner, 15, &o->ledger));
    for (unsigned i = 0; i < 3; i++) { o->nodes[i] = i + 1; memset(o->checksums[i], i + 31, EFS_HASH_SIZE); }
}
static void retire(struct efs_kv *kv, struct efs_pack_object *o)
{
    assert(!efs_pack_admission_cancel(kv, o));
    assert(efs_pack_admission_retire(kv, o) == EFS_ERR_BUSY);
    assert(efs_pack_admission_ack(kv, o, 1, 1) == EFS_ERR_BUSY);
    for (unsigned i = 0; i < 4; i++) assert(!efs_pack_admission_ack(kv, o, 1u << i, 0));
    for (unsigned i = 0; i < 4; i++) assert(!efs_pack_admission_ack(kv, o, 1u << i, 1));
}
static void run(uint32_t group)
{
    uint32_t shard = group ? 2 : 1;
    char dir[] = "/tmp/efs-pack-admission-XXXXXX"; assert(mkdtemp(dir));
    struct efs_kv *kv = efs_kv_lsm_open(dir, NULL); assert(kv);
    struct efs_pack_object a, b, actual; struct efs_pack_ref refs[2]; make(&a, refs, 1, group);
    assert(!efs_pack_admission_validate(&a, refs, 2));
    uint8_t wire[EFS_PACK_UPLOAD_MAX], saved[EFS_PACK_UPLOAD_MAX]; size_t bytes;
    assert(!efs_pack_upload_encode(&a, refs, 2, wire, sizeof(wire), &bytes));
    memcpy(saved, wire, bytes); uint32_t count;
    struct efs_pack_ref decoded[EFS_PACK_MAX_RECORDS];
    assert(!efs_pack_upload_decode(wire, bytes, &actual, decoded, EFS_PACK_MAX_RECORDS, &count));
    assert(count == 2 && !memcmp(decoded, refs, 2 * sizeof(*refs)));
    for (size_t n = 0; n < bytes; n++) assert(efs_pack_upload_decode(wire, n, &actual, decoded, EFS_PACK_MAX_RECORDS, &count));
    for (size_t n = 0; n < bytes; n++) {
        memcpy(wire, saved, bytes); wire[n] ^= 1;
        assert(efs_pack_upload_decode(wire, bytes, &actual, decoded, EFS_PACK_MAX_RECORDS, &count));
    }

    assert(efs_pack_admit(kv, &a, refs, 2) != EFS_OK);
    assert(!efs_session_create(kv, a.ledger.owner.client_uuid, 7));
    assert(!efs_session_register(kv, a.ledger.owner.client_uuid, 7, shard));
    assert(!efs_session_establish(kv, shard, a.ledger.owner.client_uuid, 7));
    assert(!efs_pack_admit(kv, &a, refs, 2)); assert(!efs_pack_admit(kv, &a, refs, 2));
    assert(!efs_pack_admission_get(kv, &a, &actual)); assert(actual.ledger.phase == EFS_PACK_ACTIVE);
    struct efs_pack_object bad = a; bad.checksums[0][0] ^= 1;
    assert(efs_pack_admission_get(kv, &bad, &actual) == EFS_ERR_PROTO);
    refs[1].ino = refs[0].ino; assert(efs_pack_admit(kv, &a, refs, 2) == EFS_ERR_INVAL);
    make(&a, refs, 1, group);
    assert(efs_pack_admission_cancel(kv, &a) != EFS_OK);
    for (uint64_t seq = 2; seq <= EFS_PACK_ADMISSION_MAX; seq++) { make(&b, refs, seq, group); assert(!efs_pack_admit(kv, &b, refs, 2)); }
    make(&b, refs, EFS_PACK_ADMISSION_MAX + 1, group);
    assert(efs_pack_admit(kv, &b, refs, 2) == EFS_ERR_BUSY);
    /* Abrupt recovery preserves permits before any lifetime transitions. */
    efs_kv_lsm_close(kv); kv = efs_kv_lsm_open(dir, NULL); assert(kv);
    assert(!efs_pack_admission_get(kv, &a, &actual));
    assert(!efs_session_begin_fence(kv, a.ledger.owner.client_uuid));
    assert(!efs_session_fence_local(kv, shard, a.ledger.owner.client_uuid, 8));
    assert(!efs_session_ack_fence(kv, a.ledger.owner.client_uuid, shard));
    assert(!efs_session_finish_fence_epoch(kv, a.ledger.owner.client_uuid, 8));
    make(&b, refs, 2, group); retire(kv, &b);
    assert(efs_pack_admission_retire(kv, &b) == EFS_ERR_BUSY); /* Can't leap earlier outstanding owner. */
    retire(kv, &a); assert(!efs_pack_admission_retire(kv, &a)); assert(!efs_pack_admission_retire(kv, &a));
    assert(efs_pack_admission_get(kv, &a, &actual) == EFS_ERR_STALE);
    assert(!efs_pack_admission_retire(kv, &b));
    assert(efs_pack_admission_get(kv, &b, &actual) == EFS_ERR_STALE);
    assert(!efs_kv_lsm_compact(kv)); efs_kv_lsm_close(kv); kv = efs_kv_lsm_open(dir, NULL); assert(kv);
    assert(efs_pack_admission_get(kv, &a, &actual) == EFS_ERR_STALE);
    make(&a, refs, 1, group); assert(efs_pack_admit(kv, &a, refs, 2) == EFS_ERR_STALE);
    efs_kv_lsm_close(kv); assert(!nftw(dir, cleanup, 32, FTW_DEPTH | FTW_PHYS));
    printf("pack admission group=%u: immutable binding, bounded permits, revocation, fence-before-delete, ordered floors and recovery PASS\n", group);
}

int main(void) { run(0); run(2); return 0; }
