/* K105: after the packer fails to read a candidate's fragments it asks
 * efs_meta_pack_candidate_alive whether the file is still the live inode with
 * the source row the pass read. A file unlinked or republished after the pass
 * read it has its fragments reclaimed (that is not a data loss and not a
 * packer read error); a file still live and unchanged is. Mem KV. */
#include "efs/pack_mapping.h"
#include "efs/meta_apply.h"
#include "efs/kv.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define T0 1000000000000000000ull
static const struct efs_meta_attrs at = {1000, 1000, T0};

static void publish(struct efs_kv *kv, efs_ino_t ino, uint64_t expected, uint64_t gen, uint64_t size)
{
    struct efs_meta_pub p;
    memset(&p, 0, sizeof(p));
    p.ino = ino; p.new_size = size; p.expected_gen = expected; p.candidate_gen = gen;
    p.coding_profile_id = EFS_META_PROFILE_K2F1;
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        p.ch.nodes[i] = (efs_node_id_t)(i + 1);
        p.ch.checksums[i][0] = (uint8_t)gen; p.ch.checksums[i][1] = (uint8_t)i;
    }
    assert(efs_meta_apply_publish(kv, &p) == EFS_OK);
}
/* A published file of the packer's group, read as a pass reads it. */
static efs_ino_t make(struct efs_kv *kv, const char *prefix, uint64_t gen, char *name, size_t cap,
                      struct efs_pack_candidate *seen)
{
    for (int i = 0; i < 4096; i++) {
        efs_ino_t ino = 0;
        snprintf(name, cap, "%s%d", prefix, i);
        assert(efs_meta_apply_create_file(kv, &at, EFS_ROOT_INO, S_IFREG | 0644, name, &ino) == EFS_OK);
        if (efs_raft_shard_group(efs_kv_lane_shard(ino, 0)) != EFS_RAFT_GROUP_SHARD) continue;
        struct efs_meta_row r;
        assert(efs_meta_apply_get_inode(kv, ino, &r) == EFS_OK);
        publish(kv, ino, 0, gen, 200);
        assert(efs_meta_pack_candidate(kv, ino, r.generation, seen) == EFS_OK);
        return ino;
    }
    assert(0);
    return 0;
}

int main(void)
{
    struct efs_kv *kv = efs_kv_mem_create();
    assert(kv && efs_meta_apply_init(kv, T0) == EFS_OK);
    struct efs_pack_candidate *seen = calloc(1, sizeof(*seen));
    char name[48];
    assert(seen);

    /* Live and unchanged: a failed fragment read of it is a real error. */
    efs_ino_t a = make(kv, "live", 0x100, name, sizeof(name), seen);
    assert(seen->ino == a && efs_meta_pack_candidate_alive(kv, seen) == EFS_OK);

    /* Republished after the read (new generation, same inode): superseded. */
    efs_ino_t b = make(kv, "repub", 0x200, name, sizeof(name), seen);
    publish(kv, b, 0x200, 0x201, 300);
    assert(efs_meta_pack_candidate_alive(kv, seen) == EFS_ERR_STALE);

    /* Hard-linked, then one name unlinked: still the live inode. */
    efs_ino_t d = make(kv, "linked", 0x400, name, sizeof(name), seen);
    assert(efs_meta_apply_link(kv, EFS_ROOT_INO, name, EFS_ROOT_INO, "second-name", T0 + 1) == EFS_OK);
    assert(efs_meta_apply_unlink(kv, EFS_ROOT_INO, name, T0 + 2) == EFS_OK);
    assert(seen->ino == d && efs_meta_pack_candidate_alive(kv, seen) == EFS_OK);

    /* Unlinked after the read: gone. */
    efs_ino_t c = make(kv, "gone", 0x300, name, sizeof(name), seen);
    assert(seen->ino == c && efs_meta_pack_candidate_alive(kv, seen) == EFS_OK);
    assert(efs_meta_apply_unlink(kv, EFS_ROOT_INO, name, T0 + 3) == EFS_OK);
    assert(efs_meta_pack_candidate_alive(kv, seen) == EFS_ERR_STALE);

    free(seen);
    printf("pack candidate alive: OK\n");
    return 0;
}
