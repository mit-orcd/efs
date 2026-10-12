#ifndef EFS_QUERY_STAGE_TRACE_H
#define EFS_QUERY_STAGE_TRACE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Opt-in bounded stage records. Numeric counters only: no names or values.
 * Each event belongs to an existing bounded capture/build attempt. */
static inline int efs_query_stage_enabled(void)
{
    const char *enabled=getenv("EFS_QUERY_STAGE_TRACE");
    return enabled && !strcmp(enabled,"1");
}
static inline void efs_query_stage_trace(const char *stage,int rc,uint64_t elapsed,
                                         uint64_t before,uint64_t after)
{
    if(efs_query_stage_enabled())
        fprintf(stderr,"query-stage: stage=%s rc=%d elapsed_ms=%llu before=%llu after=%llu\n",
            stage,rc,(unsigned long long)elapsed,(unsigned long long)before,
            (unsigned long long)after);
}
static inline void efs_query_root_stage_trace(const char *stage,int rc,uint64_t elapsed,
    uint64_t root,uint64_t generation,uint64_t inode)
{
    if(efs_query_stage_enabled())
        fprintf(stderr,"query-stage: stage=%s rc=%d elapsed_ms=%llu before=%llu after=%llu inode=%llu\n",
            stage,rc,(unsigned long long)elapsed,(unsigned long long)root,
            (unsigned long long)generation,(unsigned long long)inode);
}

#endif
