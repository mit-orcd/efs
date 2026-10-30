/* Actual ordinary PUT/rebuild, durable session guard, physical store and
 * superseded deletion; only metadata responses and export refs are fixtures. */
#define main reclaim_fixture_main
#include "test_reclaim.c"
#undef main
#include "efs/put_ticket.h"
#include "../src/server/put_fence.c"
#include "../src/server/store_nvme.c"
#include "../src/server/put_upload.c"

static struct efs_put_ticket ordinary_ticket;
static int ticket_live = 1, mark_live = 1;
static uint8_t *ordinary_frame, *rebuild_frame;
static uint32_t ordinary_len, rebuild_len;
static int ordinary_rebuild;
int server_stop_requested(void) { return 0; }
void server_raft_host_submit(const uint8_t *p, uint32_t n, struct efs_msg_raft_mkfs_reply *r)
{
    assert(n == 2 + EFS_PUT_TICKET_WIRE_LEN && p[1] == EFS_MD_REQ_PUT_TICKET_STATUS);
    memset(r, 0, sizeof(*r));
    r->rc = ticket_live ? EFS_OK : EFS_ERR_NOT_FOUND;
    r->salt = EFS_PUT_TICKET_ADMITTED;
    capture_segment_authority();
}
int server_raft_host_degraded_check(const uint8_t mark[EFS_REBUILD_MARK_LEN])
{
    struct efs_meta_degraded d;
    assert(!server_degraded_mark_decode(mark, &d));
    assert(d.ino == ordinary_ticket.ino && d.object_gen == ordinary_ticket.object);
    int rc = mark_live ? EFS_OK : EFS_ERR_NOT_FOUND;
    capture_segment_authority();
    return rc;
}
static void ordinary_setup(void)
{
    struct efs_meta_pub p = {.ino = 4097, .inode_gen = 7,
        .coding_profile_id = EFS_META_PROFILE_K2F1, .fresh_object = 1,
        .put_fragment_len = EFS_FRAGMENT_SIZE, .put_member_mask = 7};
    p.put_id.client_uuid[0] = 1; p.put_id.session_epoch = 1; p.put_id.seq = 1;
    p.publication_id = p.put_id;
    p.candidate_gen = efs_meta_candidate_gen(p.put_id.client_uuid, 1, 1, 0, 0);
    ordinary_len = EFS_PUT_UPLOAD_HEADER_LEN + p.put_fragment_len;
    ordinary_frame = malloc(ordinary_len); assert(ordinary_frame);
    memset(ordinary_frame + EFS_PUT_UPLOAD_HEADER_LEN, 91, p.put_fragment_len);
    for (unsigned i = 0; i < 3; i++) {
        p.ch.nodes[i] = i + 1;
        efs_hash(ordinary_frame + EFS_PUT_UPLOAD_HEADER_LEN, p.put_fragment_len, p.ch.checksums[i]);
    }
    assert(!efs_put_ticket_from_pub(&p, &ordinary_ticket));
    assert(!efs_put_upload_header(&ordinary_ticket, 1, 0, ordinary_frame));
    struct efs_meta_degraded d = {.ino = p.ino, .inode_gen = p.inode_gen,
        .object_gen = ordinary_ticket.object, .chunk_index = 0, .lane = 0, .missing_mask = 1};
    memcpy(d.nodes, ordinary_ticket.nodes, sizeof(d.nodes));
    memcpy(d.checksums, ordinary_ticket.checksums, sizeof(d.checksums));
    rebuild_len = EFS_REBUILD_PUT_HEADER_LEN + p.put_fragment_len;
    rebuild_frame = malloc(rebuild_len); assert(rebuild_frame);
    efs_put_upload_wr32(rebuild_frame, 1); server_degraded_mark_encode(rebuild_frame + 4, &d);
    memcpy(rebuild_frame + EFS_REBUILD_PUT_HEADER_LEN,
           ordinary_frame + EFS_PUT_UPLOAD_HEADER_LEN, p.put_fragment_len);
}
static void *ordinary_writer(void *unused)
{
    (void)unused;
    capture_this_put = 1; efs_tls_write_root = 0;
    uint8_t path;
    delayed_put_rc = ordinary_rebuild
        ? server_rebuild_put(&S, rebuild_frame, rebuild_len, &path)
        : server_put_upload(&S, ordinary_frame, ordinary_len, &path);
    return NULL;
}
static void ordinary_delayed_authority(void)
{
    boot(); ordinary_setup();
    S.nwriters = 1; /* the real writer pool: the retire mark must cross to its thread */
    assert(server_writer_pool_start(&S) == 0);
    uint64_t base = used();
    uint8_t placed;
    efs_tls_write_root = 0;
    assert(!server_put_upload(&S, ordinary_frame, ordinary_len, &placed));
    authority_seen = authority_release = 0;
    pthread_t writer; assert(!pthread_create(&writer, NULL, ordinary_writer, NULL));
    pthread_mutex_lock(&authority_mu);
    while (!authority_seen) pthread_cond_wait(&authority_cv, &authority_mu);
    pthread_mutex_unlock(&authority_mu);
    pthread_rwlock_t *guard = server_inode_guard(1, ordinary_ticket.ino);
    int before = pthread_rwlock_trywrlock(guard);
    assert(before == 0 || before == EBUSY);
    if (!before) pthread_rwlock_unlock(guard);
    ticket_live = mark_live = 0; /* committed map replacement / mark retirement */
    if (!before)
        assert(!server_gc_superseded_delete(&S, 1, ordinary_ticket.ino, 0, 0,
            ordinary_ticket.object, ordinary_ticket.checksums[0], 1));
    pthread_mutex_lock(&authority_mu); authority_release = 1;
    pthread_cond_broadcast(&authority_cv); pthread_mutex_unlock(&authority_mu);
    assert(!pthread_join(writer, NULL));
    assert(delayed_put_rc == EFS_OK || delayed_put_rc == EFS_ERR_STALE || delayed_put_rc == EFS_ERR_NOT_FOUND);
    if (before == EBUSY)
        assert(!server_gc_superseded_delete(&S, 1, ordinary_ticket.ino, 0, 0,
            ordinary_ticket.object, ordinary_ticket.checksums[0], 1));
    char path[8192]; efs_tls_chunk_gen = ordinary_ticket.object;
    fragment_path_at(&S, 0, &EX, ordinary_ticket.ino, 0, 0, path, sizeof(path));
    assert(access(path, F_OK) && errno == ENOENT);
    assert(used() == base);
    free(ordinary_frame); free(rebuild_frame);
}
int main(int argc, char **argv)
{
    assert(argc == 2 && (!strcmp(argv[1], "put") || !strcmp(argv[1], "rebuild")));
    ordinary_rebuild = !strcmp(argv[1], "rebuild");
    efs_test_path(roots[0], sizeof(roots[0]), "efs-ba-upload-a-XXXXXX");
    efs_test_path(roots[1], sizeof(roots[1]), "efs-ba-upload-b-XXXXXX");
    assert(efs_test_mkdtemp(roots[0]) && efs_test_mkdtemp(roots[1]));
    run(argv[1], ordinary_delayed_authority, 0);
    return 0;
}
