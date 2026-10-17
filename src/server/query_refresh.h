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

/* A rejected coherent-cut capture does no disk build. Give short captures a
 * bounded chance to find a quiet cut, without accelerating authority/import
 * refusals or expensive captures. This is scheduling only: every attempt
 * retains fresh pin/proofs/fences, the bounded capture burst and original deadline. */
#define EFS_QUERY_CUT_RETRY_MS 20u
#define EFS_QUERY_CUT_CHEAP_MS 200u
#define EFS_QUERY_PENDING_CHEAP_MS 2000u
/* Cheap refusals acquire no candidate disk files. Poll more frequently only
 * while seeking a coherent cut; success, authority loss and expensive errors
 * restore normal publisher polling. Neither safety proof nor request deadline
 * changes. Each capture retains its separate 100 ms / 16 attempt bound. */
static inline unsigned efs_query_refresh_poll_ms(int cut_retry)
{
    return cut_retry?EFS_QUERY_CUT_RETRY_MS:100u;
}
static inline unsigned efs_query_refresh_admit_ms(int cut_retry)
{
    return cut_retry?EFS_QUERY_CUT_RETRY_MS:200u;
}
static inline int efs_query_refresh_cut_retry(int rc,int transaction_changed,uint64_t capture_ms)
{
    return rc==EFS_ERR_BUSY && transaction_changed && capture_ms<EFS_QUERY_CUT_CHEAP_MS;
}
/* A confirmed pending-row preflight also rejects without candidate I/O.
 * Confirmed blockers create no candidate files. Their prefix scan can exceed
 * a cheap capture on large catalogs; allow up to the normal freshness interval
 * before restoring normal backoff. Errors, shutdown/scan BUSY and still more
 * expensive checks retain normal scheduling; the sole publisher bounds CPU
 * concurrency and every retry acquires a new immutable source. */
static inline int efs_query_refresh_preflight_retry(int rc,int pending,uint64_t build_ms)
{
    return rc==EFS_ERR_BUSY && pending && build_ms<EFS_QUERY_PENDING_CHEAP_MS;
}
/* EFS_QUERY_REFRESH_MS: minimum time between catalog refresh attempts and
 * the pending age reported as overdue. Unset keeps the default; invalid text
 * or a value outside 200..3600000 keeps it and returns EFS_ERR_INVAL. A
 * longer interval trades catalog freshness for fewer full-shard rebuilds. */
#define EFS_QUERY_REFRESH_MIN_MS 200u
#define EFS_QUERY_REFRESH_MAX_MS 3600000u
static inline int efs_query_refresh_interval_config(const char *text,unsigned fallback,
                                                    unsigned *ms)
{
    *ms=fallback;
    if(!text)return EFS_OK;
    unsigned long long v=0;
    if(!*text)return EFS_ERR_INVAL;
    for(const char *p=text;*p;p++){
        if(*p<'0' || *p>'9')return EFS_ERR_INVAL;
        v=v*10+(unsigned)(*p-'0');
        if(v>EFS_QUERY_REFRESH_MAX_MS)return EFS_ERR_INVAL;
    }
    if(v<EFS_QUERY_REFRESH_MIN_MS)return EFS_ERR_INVAL;
    *ms=(unsigned)v;return EFS_OK;
}
static inline int efs_query_refresh_due_reason(uint64_t now,uint64_t pending_since,
    uint64_t last_attempt,uint64_t dirty,int initial,int failed,int cut_retry,
    uint64_t trigger,uint64_t interval)
{
    if(cut_retry && failed && (dirty || initial) && last_attempt)
        return now-last_attempt>=EFS_QUERY_CUT_RETRY_MS;
    return efs_query_refresh_due(now,pending_since,last_attempt,dirty,initial,failed,trigger,interval);
}

#endif
