#ifndef EFS_SPARSE_METADATA_H
#define EFS_SPARSE_METADATA_H
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/mman.h>
#endif

/* Sparse metadata slots should cost touched base pages, not entire THPs.
 * This is a range hint before publication, never a process-wide allocator
 * mode, a cache-capacity change, or a discard of existing bytes. */
static inline int efs_sparse_metadata_pages(void *address, size_t bytes)
{
#if defined(__linux__) && defined(MADV_NOHUGEPAGE)
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t first = (uintptr_t)address;
    if (page <= 0 || !bytes || bytes > UINTPTR_MAX-first)
        return -1;
    uintptr_t last = first+bytes;
    uintptr_t remainder = last % (uintptr_t)page;
    if (remainder) {
        uintptr_t extra = (uintptr_t)page-remainder;
        if (last > UINTPTR_MAX-extra) return -1;
        last += extra;
    }
    first -= first % (uintptr_t)page;
    return madvise((void *)first, (size_t)(last-first), MADV_NOHUGEPAGE);
#else
    (void)address; (void)bytes;
    return 0;
#endif
}
#endif
