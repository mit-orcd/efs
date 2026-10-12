#ifndef EFS_RDMA_MEMCHECK_H
#define EFS_RDMA_MEMCHECK_H

#include <stddef.h>

/* NIC writes do not execute CPU stores. These annotations are opt-in for
 * Memcheck builds; ordinary builds have no Valgrind dependency or requests. */
#if defined(EFS_MEMCHECK) && EFS_MEMCHECK
#include <valgrind/memcheck.h>
#endif

static inline void rdma_memcheck_post_recv(void *buffer, size_t capacity)
{
#if defined(EFS_MEMCHECK) && EFS_MEMCHECK
    /* Reposting must invalidate old bytes, including a previous longer frame. */
    VALGRIND_MAKE_MEM_UNDEFINED(buffer, capacity);
#else
    (void)buffer; (void)capacity;
#endif
}

static inline void rdma_memcheck_recv_complete(void *buffer, size_t capacity,
                                               size_t received)
{
#if defined(EFS_MEMCHECK) && EFS_MEMCHECK
    /* Only a successful, bounded completion establishes NIC-written bytes. */
    if (received <= capacity)
        VALGRIND_MAKE_MEM_DEFINED(buffer, received);
#else
    (void)buffer; (void)capacity; (void)received;
#endif
}

static inline void rdma_memcheck_send(const void *buffer, size_t length)
{
#if defined(EFS_MEMCHECK) && EFS_MEMCHECK
    /* Hardware cannot propagate Memcheck's shadow bits to the receiver. Check
     * every transmitted byte here, before declaring received DMA bytes valid. */
    VALGRIND_CHECK_MEM_IS_DEFINED(buffer, length);
#else
    (void)buffer; (void)length;
#endif
}
#endif
