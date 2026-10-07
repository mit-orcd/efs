#include "client_internal.h"
#include "efs/publication_ack.h"
/* The queue owns immutable requests independently of cache entry lifetime.
 * Consuming a terminal result is not retirement: unknown ACKs retain budget. */
struct efs_publication_ack_queue *efs_client_publication_ack_alloc(void)
{
    return efs_buf_metadata_alloc(sizeof(struct efs_publication_ack_queue));
}
int efs_client_publication_ack_free(struct efs_publication_ack_queue *queue)
{
    if (!queue) return EFS_OK;
    if (queue->count) return EFS_ERR_BUSY;
    efs_buf_metadata_free(queue,sizeof(*queue));
    return EFS_OK;
}
