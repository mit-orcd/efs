#ifndef EFS_QUERY_STAGE_TRACE_H
#define EFS_QUERY_STAGE_TRACE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Opt-in bounded stage records. Numeric counters only: no names or values.
 * Each event belongs to an existing bounded capture/build attempt. */
static inline void efs_query_stage_trace(const char *stage,int rc,uint64_t elapsed,
                                         uint64_t before,uint64_t after)
{
    const char *enabled=getenv("EFS_QUERY_STAGE_TRACE");
    if(enabled && !strcmp(enabled,"1"))
        fprintf(stderr,"query-stage: stage=%s rc=%d elapsed_ms=%llu before=%llu after=%llu\n",
            stage,rc,(unsigned long long)elapsed,(unsigned long long)before,
            (unsigned long long)after);
}
#endif
