#define _XOPEN_SOURCE 700
#include "efs/pack_admission.h"
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include "efs/session.h"
#include "efs/publication.h"
#include "efs/raft.h"
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const struct efs_kv_ops *real_ops;
static int fail_batch;
static int fault_batch(void *context, const struct efs_kv_item *items, uint32_t n)
{
    if (fail_batch) { fail_batch = 0; return EFS_ERR_IO; }
    return real_ops->batch(context, items, n);
}
static int cleanup(const char *p, const struct stat *s, int t, struct FTW *f)
{ (void)s; (void)t; (void)f; return remove(p); }
static void run(unsigned scenario)
{
    char dir[] = "/tmp/efs-packed-publication-XXXXXX"; assert(mkdtemp(dir));
    struct efs_kv *kv = efs_kv_lsm_open(dir, NULL); assert(kv); assert(!efs_meta_apply_init(kv, 100));
    struct efs_opid owner = {.client_uuid = {31}, .session_epoch = 3, .seq = 41};
    uint64_t id, gen; assert(!efs_pack_identity(&owner, 0, &id, &gen));
    uint8_t payload[512] = {71}, image[EFS_SEGMENT_MAX_SIZE]; size_t geometry;
    struct efs_pack_record records[2]; struct efs_meta_pub pubs[2] = {0};
    const struct efs_meta_attrs attrs = {1000, 1000, 100};
    for (unsigned i = 0; i < 2; i++) {
        char name[32]; snprintf(name, sizeof(name), "fresh-pack-%u", i);
        efs_ino_t ino; struct efs_meta_row row;
        assert(!efs_meta_apply_create_file(kv, &attrs, EFS_ROOT_INO, S_IFREG | 0600, name, &ino));
        assert(!efs_meta_apply_get_inode(kv, ino, &row));
        uint32_t shard = efs_kv_lane_shard(ino, 0); assert(efs_raft_shard_group(shard) == 0);
        assert(!efs_session_establish(kv, shard, owner.client_uuid, owner.session_epoch));
        pubs[i].publication_id = owner; pubs[i].publication_id.seq = 51 + i;
        pubs[i].ino = ino; pubs[i].inode_gen = row.generation; pubs[i].new_size = 512;
        pubs[i].candidate_gen = efs_meta_candidate_gen(owner.client_uuid, 3, 51 + i, 0, 0);
        pubs[i].coding_profile_id = EFS_META_PROFILE_PACK_K2F1; pubs[i].content_epoch = row.content_epoch;
        pubs[i].fresh_object = pubs[i].durable_result = 1;
        pubs[i].receipt_locator = (struct efs_publication_locator){0, 101 + i, 1};
        records[i] = (struct efs_pack_record){ino, row.generation, row.content_epoch, pubs[i].candidate_gen, 512, payload};
    }
    struct efs_pack_ref refs[2]; assert(!efs_pack_build(id, gen, 0, records, 2, image, sizeof(image), &geometry, refs));
    struct efs_pack_object object = {0}, current;
    assert(!efs_pack_ledger_init(image, geometry, refs, 2, &owner, 15, &object.ledger));
    for (unsigned i = 0; i < 3; i++) { object.nodes[i] = i + 1; memset(object.checksums[i], 57 + i, 32); }
    assert(!efs_session_establish(kv, 1, owner.client_uuid, owner.session_epoch));
    assert(!efs_pack_admit(kv, &object, refs, 2));
    for (unsigned i = 0; i < 2; i++) {
        pubs[i].ch.packed = refs[i]; memcpy(pubs[i].ch.nodes, object.nodes, sizeof(object.nodes));
        memcpy(pubs[i].ch.checksums, object.checksums, sizeof(object.checksums));
        if (!i) assert(!efs_meta_apply_publication_admit(kv, pubs + i));
        else {
            if(scenario==4)assert(!efs_meta_apply_content_fence(kv,pubs[i].ino,pubs[i].inode_gen,EFS_META_FENCE_INODE,pubs[i].content_epoch,pubs[i].content_epoch+1,0,110));
            /* Direct terminal publication must not expose a mapping, receipt or
             * LIVE reference when its one atomic storage batch fails. */
            real_ops=kv->ops;struct efs_kv_ops fault_ops=*real_ops;
            fault_ops.batch=fault_batch;kv->ops=&fault_ops;fail_batch=1;
            assert(efs_meta_apply_publish(kv,pubs+i)==EFS_ERR_IO);
            kv->ops=real_ops;
            int verdict;
            assert(efs_meta_publication_result(kv,pubs+i,&verdict)==EFS_ERR_NOT_FOUND);
            struct efs_meta_chunk absent;
            assert(efs_meta_apply_get_chunk(kv,pubs[i].ino,0,&absent)==EFS_ERR_NOT_FOUND);
            assert(!efs_pack_admission_get(kv,&object,&current));
            assert(current.ledger.state[0]==EFS_PACK_LIVE && current.ledger.state[1]==EFS_PACK_PENDING);
        }
        int expected=scenario==4 && i==1?EFS_ERR_STALE:EFS_OK;
        assert(efs_meta_apply_publish(kv, pubs + i)==expected); assert(efs_meta_apply_publish(kv, pubs + i)==expected);
        int verdict; struct efs_publication_locator locator;
        assert(!efs_meta_publication_receipt(kv, pubs + i, &verdict, &locator) && verdict==expected && locator.index == 101 + i);
        if(expected) {
            struct efs_meta_chunk absent;assert(efs_meta_apply_get_chunk(kv,pubs[i].ino,0,&absent)==EFS_ERR_NOT_FOUND);
            assert(!efs_pack_admission_get(kv,&object,&current));
            assert(current.ledger.state[0]==EFS_PACK_LIVE&&current.ledger.state[1]==EFS_PACK_CANCELLED);
            continue;
        }
        struct efs_meta_chunk mapped; assert(!efs_meta_apply_get_chunk(kv, pubs[i].ino, 0, &mapped));
        assert(mapped.coding_profile_id == EFS_META_PROFILE_PACK_K2F1 && mapped.packed.segment_id == id);
        assert(!efs_pack_admission_get(kv, &object, &current));
        assert(current.ledger.state[i] == EFS_PACK_LIVE && (i || current.ledger.state[1] == EFS_PACK_PENDING));
    }
    assert(!efs_pack_upload_retire(kv, &object)); assert(!efs_pack_upload_retire(kv, &object));
    assert(efs_pack_admission_get(kv, &object, &current) == EFS_ERR_STALE);
    assert(!efs_pack_object_get(kv, 0, id, gen, &current));
    assert(current.ledger.state[0] == EFS_PACK_LIVE && current.ledger.state[1] == (scenario==4?EFS_PACK_CANCELLED:EFS_PACK_LIVE));
    uint8_t digest[32], changed[32]; assert(!efs_publication_digest(pubs, digest));
    struct efs_meta_pub bad = pubs[0]; bad.ch.packed.digest[0] ^= 1; assert(!efs_publication_digest(&bad, changed));
    assert(memcmp(digest, changed, 32)); assert(efs_meta_apply_publish(kv, &bad) == EFS_ERR_PROTO);
    assert(!efs_kv_lsm_compact(kv)); efs_kv_lsm_close(kv); kv = efs_kv_lsm_open(dir, NULL); assert(kv);
    assert(!efs_meta_apply_publish(kv, pubs)); assert(!efs_pack_object_get(kv, 0, id, gen, &current));
    assert(current.ledger.state[0] == EFS_PACK_LIVE && current.ledger.state[1] == (scenario==4?EFS_PACK_CANCELLED:EFS_PACK_LIVE));
    if(scenario==4) {
        assert(efs_meta_apply_publish(kv,pubs+1)==EFS_ERR_STALE);
        assert(!efs_meta_apply_lane_sweep(kv,pubs[0].ino,pubs[0].inode_gen,0));
        assert(!efs_pack_object_get(kv,0,id,gen,&current)&&current.ledger.phase==EFS_PACK_RECLAIMING);
        efs_kv_lsm_close(kv);assert(!nftw(dir,cleanup,32,FTW_DEPTH|FTW_PHYS));
        puts("packed rejection: atomic receipt/cancellation, failed-batch retention, live neighbor, upload retirement and cold GC eligibility PASS");return;
    }
    uint32_t ci = 0;
    if (!scenario) {
        real_ops = kv->ops; struct efs_kv_ops fault_ops = *real_ops; fault_ops.batch = fault_batch;
        kv->ops = &fault_ops; fail_batch = 1;
        assert(efs_meta_apply_lane_sweep_batch(kv, pubs[0].ino, pubs[0].inode_gen, 0, &ci, 1) == EFS_ERR_IO);
        kv->ops = real_ops;
        assert(!efs_pack_object_get(kv, 0, id, gen, &current));
        assert(current.ledger.state[0] == EFS_PACK_LIVE && current.ledger.state[1] == EFS_PACK_LIVE);
        struct efs_meta_chunk retained; assert(!efs_meta_apply_get_chunk(kv, pubs[0].ino, 0, &retained));
        assert(retained.packed.segment_id == id);
        assert(!efs_meta_apply_lane_sweep_batch(kv, pubs[0].ino, pubs[0].inode_gen, 0, &ci, 1));
        assert(!efs_meta_apply_lane_sweep_batch(kv, pubs[0].ino, pubs[0].inode_gen, 0, &ci, 1));
    } else if (scenario == 1) {
        struct efs_meta_truncate truncate = {.expect_gen = pubs[0].inode_gen, .size = 0, .lane_mask = 1};
        assert(!efs_meta_apply_truncate(kv, pubs[0].ino, 110, &truncate));
        struct efs_meta_row row; assert(!efs_meta_apply_get_inode(kv, pubs[0].ino, &row));
        assert(!row.base_size && row.content_epoch > pubs[0].content_epoch);
    } else if (scenario == 2) {
        assert(!efs_meta_apply_lane_fence(kv, pubs[0].ino, pubs[0].inode_gen, 0, 2, 0, 0, 0));
        assert(!efs_meta_apply_lane_fence(kv, pubs[0].ino, pubs[0].inode_gen, 0, 2, 0, 0, 0));
    } else {
        /* Retain the base behind a zero-length fence, then materialize its
         * verified empty view. The ledger retires with the actual deletion. */
        assert(!efs_meta_apply_content_fence(kv, pubs[0].ino, pubs[0].inode_gen, 0, pubs[0].content_epoch, pubs[0].content_epoch + 1, 0, 110));
        struct efs_meta_chunk_view view;
        assert(!efs_meta_get_chunk_view(kv, pubs[0].ino, pubs[0].inode_gen, 0, EFS_MIN_CHUNK_SIZE, &view));
        assert(!efs_meta_apply_sweep_chunk(kv, pubs[0].ino, pubs[0].inode_gen, 0, &view, NULL));
    }
    assert(!efs_pack_object_get(kv, 0, id, gen, &current));
    assert(current.ledger.state[0] == EFS_PACK_RELEASED && current.ledger.state[1] == EFS_PACK_LIVE);
    struct efs_meta_chunk neighbor;
    assert(!efs_meta_apply_get_chunk(kv, pubs[1].ino, 0, &neighbor));
    assert(neighbor.packed.segment_id == id);
    assert(!efs_kv_lsm_compact(kv)); efs_kv_lsm_close(kv); kv = efs_kv_lsm_open(dir, NULL); assert(kv);
    assert(!efs_pack_object_get(kv, 0, id, gen, &current));
    assert(current.ledger.state[0] == EFS_PACK_RELEASED && current.ledger.state[1] == EFS_PACK_LIVE);
    assert(!efs_meta_apply_lane_sweep(kv, pubs[1].ino, pubs[1].inode_gen, 0));
    assert(!efs_pack_object_get(kv, 0, id, gen, &current));
    assert(current.ledger.phase == EFS_PACK_RECLAIMING);
    efs_kv_lsm_close(kv); assert(!nftw(dir, cleanup, 32, FTW_DEPTH | FTW_PHYS));
    printf("packed publication: admission-bound mapping/receipt, exact replay, retirement scenario %u, neighbor retention and cold restart PASS\n", scenario);
}

int main(void) { for (unsigned i = 0; i < 5; i++) run(i); return 0; }
