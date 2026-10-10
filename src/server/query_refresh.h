#ifndef EFS_QUERY_REFRESH_H
#define EFS_QUERY_REFRESH_H
#include <stdint.h>
/* A derived-cache refresh is not a consensus/checkpoint barrier. Coalesce
 * admission once per existing freshness interval, regardless of a busy N
 * trigger, so a faster incremental builder cannot churn generations nonstop.
 * Initial publication stays prompt; failed attempts keep the same backoff.
 * Overdue/age reporting remains independent of this scheduling decision. */
static inline int efs_query_refresh_due(uint64_t now,uint64_t pending_since,
    uint64_t last_attempt,uint64_t dirty,int initial,int failed,
    uint64_t trigger,uint64_t interval)
{
    if(!dirty && !initial)return 0;
    if(failed && last_attempt && now-last_attempt<interval)return 0;
    if(initial)return 1;
    if(dirty<trigger && now-pending_since<interval)return 0;
    return !last_attempt || now-last_attempt>=interval;
}
#endif
