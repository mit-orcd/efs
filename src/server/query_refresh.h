#ifndef EFS_QUERY_REFRESH_H
#define EFS_QUERY_REFRESH_H
#include <stdint.h>
#include "efs/common.h"
/* A derived-cache refresh is not a consensus/checkpoint barrier. Coalesce
 * admission once per existing freshness interval, regardless of a busy N
 * trigger, so a faster incremental builder cannot churn generations nonstop.
 * Initial publication stays prompt; consecutive failures back off at most 30 s.
 * Overdue/age reporting remains independent of this scheduling decision. */
/* Failed derived-cache builds must not monopolize foreground storage with
 * repeated full scratch rebuilds. First retry keeps the freshness interval;
 * later failures double only the scheduling delay, bounded at 30 seconds.
 * The published error and pending-age reporting remain visible throughout. */
static inline unsigned efs_query_refresh_failures(unsigned previous,int prior,int rc)
{
    if(rc==EFS_OK)return 0;
    /* Ordinary authority/contention retries keep the normal interval. Only
     * repeated identical build failures should progressively back off. */
    if(rc==EFS_ERR_BUSY || rc==EFS_ERR_NOT_PRIMARY || rc==EFS_ERR_STALE || rc!=prior)
        return 1;
    return previous<5?previous+1:5;
}
static inline uint64_t efs_query_refresh_retry(unsigned failures,uint64_t interval)
{
    uint64_t cap=interval>30000?interval:30000,delay=interval;
    if(!delay)return 0;
    for(unsigned i=1;i<failures && delay<cap;i++)
        delay=delay>=cap-delay?cap:delay*2;
    return delay;
}
static inline int efs_query_refresh_due(uint64_t now,uint64_t pending_since,
    uint64_t last_attempt,uint64_t dirty,int initial,int failed,
    uint64_t trigger,uint64_t interval)
{
    if(!dirty && !initial)return 0;
    if(failed && last_attempt && now-last_attempt<efs_query_refresh_retry((unsigned)failed,interval))return 0;
    if(initial)return 1;
    if(dirty<trigger && now-pending_since<interval)return 0;
    return !last_attempt || now-last_attempt>=interval;
}
#endif
