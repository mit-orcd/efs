#include "efs/store.h"
#include "server_internal.h"
#include <string.h>

/* NVMe backend for the store vtable. ctx is struct efs_nvme_store bound
 * to an already-acquired export (handler holds the inflight ref). */

static int nvme_get(void *ctx, const struct efs_frag_id *id,
                    uint8_t *buf, uint32_t *len,
                    uint8_t sum[EFS_HASH_SIZE], int *sum_ok)
{
    struct efs_nvme_store *n = ctx;
    int dummy_ok = 0;
    uint8_t dummy_sum[EFS_HASH_SIZE];
    int *ok = sum_ok ? sum_ok : &dummy_ok;
    uint8_t *ck = sum ? sum : dummy_sum;

    int rc;

    if (!n || !n->s || !n->ex || !id || !len)
        return EFS_ERR_INVAL;
    if (id->export_id != n->ex->id)
        return EFS_ERR_INVAL;
    efs_tls_chunk_gen = id->chunk_generation;
    uint64_t start = efs_iostats_now_us();
    rc = server_read_fragment_with_sum(n->s, n->ex, id->ino, id->chunk_index,
                                       id->fragment_index, buf, len, ck, ok);
    efs_iostats_add(EFS_IOSTAT_DISK_READ, rc == EFS_OK ? *len : 0,
                    efs_iostats_now_us() - start, rc != EFS_OK);
    efs_tls_chunk_gen = 0;
    return rc;
}

static int nvme_put(void *ctx, const struct efs_frag_id *id,
                    const uint8_t *buf, uint32_t len,
                    const uint8_t sum[EFS_HASH_SIZE])
{
    struct efs_nvme_store *n = ctx;
    int rc;

    if (!n || !n->s || !n->ex || !id || (len > 0 && !buf))
        return EFS_ERR_INVAL;
    if (id->export_id != n->ex->id)
        return EFS_ERR_INVAL;
    efs_tls_chunk_gen = id->chunk_generation;
    if (sum)
        rc = server_write_fragment_with_sum(n->s, n->ex, id->ino,
                                            id->chunk_index,
                                            id->fragment_index, buf, len,
                                            sum);
    else
        rc = server_write_fragment_sync(n->s, n->ex, id->ino, id->chunk_index,
                                        id->fragment_index, buf, len);
    efs_tls_chunk_gen = 0;
    return rc;
}

static int nvme_del(void *ctx, const struct efs_frag_id *id)
{
    struct efs_nvme_store *n = ctx;

    if (!n || !n->s || !n->ex || !id)
        return EFS_ERR_INVAL;
    if (id->export_id != n->ex->id)
        return EFS_ERR_INVAL;
    efs_tls_chunk_gen = id->chunk_generation;
    server_unlink_fragment_files(n->s, n->ex, id->ino, id->chunk_index,
                                 id->fragment_index);
    efs_tls_chunk_gen = 0;
    return EFS_OK;
}

static int nvme_del_if_sum(void *ctx, const struct efs_frag_id *id,
                           const uint8_t expect_sum[EFS_HASH_SIZE])
{
    struct efs_nvme_store *n = ctx;
    int rc;

    if (!n || !n->s || !n->ex || !id || !expect_sum)
        return EFS_ERR_INVAL;
    if (id->export_id != n->ex->id)
        return EFS_ERR_INVAL;
    efs_tls_chunk_gen = id->chunk_generation;
    rc = server_delete_fragment_if_sum(n->s, n->ex, id->ino,
                                       id->chunk_index, id->fragment_index,
                                       expect_sum);
    efs_tls_chunk_gen = 0;
    return rc;
}

static void nvme_destroy(void *ctx)
{
    (void)ctx; /* stack-bound; caller owns s/ex */
}

static const struct efs_store_ops nvme_ops = {
    .get = nvme_get,
    .put = nvme_put,
    .del = nvme_del,
    .del_if_sum = nvme_del_if_sum,
    .destroy = nvme_destroy,
};

void efs_store_nvme_bind(struct efs_store *st, struct efs_nvme_store *ctx,
                         struct efsd_server *s, struct efs_export *ex)
{
    ctx->s = s;
    ctx->ex = ex;
    st->ops = &nvme_ops;
    st->ctx = ctx;
}
