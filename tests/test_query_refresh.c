#include "../src/server/query_refresh.h"
#include "../src/server/query_export_lease.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    assert(efs_query_refresh_due(100,0,0,0,1,0,256,2000));
    assert(!efs_query_refresh_due(9000,0,0,0,0,0,256,2000));
    /* Busy waves remain coalesced; sparse writes still use the time trigger. */
    assert(!efs_query_refresh_due(1500,1100,1000,10000,0,0,256,2000));
    assert(!efs_query_refresh_due(2999,1100,1000,256,0,0,256,2000));
    assert(efs_query_refresh_due(3000,1100,1000,256,0,0,256,2000));
    assert(!efs_query_refresh_due(3099,1100,1000,1,0,0,256,2000));
    assert(efs_query_refresh_due(3100,1100,1000,1,0,0,256,2000));
    /* Failure neither hot-loops at startup nor suppresses the later retry. */
    assert(!efs_query_refresh_due(2999,0,1000,0,1,1,256,2000));
    assert(efs_query_refresh_due(3000,0,1000,0,1,1,256,2000));
    /* A 49-second failed build backs off from its completion, not admission.
     * Retained invalid data must not drive uninterrupted full rebuilds. */
    assert(!efs_query_refresh_due(50001,0,50000,0,1,1,256,2000));
    assert(!efs_query_refresh_due(51999,0,50000,0,1,1,256,2000));
    assert(efs_query_refresh_due(52000,0,50000,0,1,1,256,2000));
    assert(efs_query_refresh_retry(1,2000)==2000);
    assert(efs_query_refresh_retry(2,2000)==4000);
    assert(efs_query_refresh_retry(3,2000)==8000);
    assert(efs_query_refresh_retry(4,2000)==16000);
    assert(efs_query_refresh_retry(5,2000)==30000);
    assert(efs_query_refresh_retry(1000,2000)==30000);
    assert(efs_query_refresh_retry(1000,UINT64_MAX)==UINT64_MAX);
    assert(!efs_query_refresh_retry(1000,0));
    assert(!efs_query_refresh_due(53999,0,50000,0,1,2,256,2000));
    assert(efs_query_refresh_due(54000,0,50000,0,1,2,256,2000));
    assert(!efs_query_refresh_due(79999,0,50000,10000,1,5,256,2000));
    assert(efs_query_refresh_due(80000,0,50000,10000,1,5,256,2000));
    /* Success resets retry scheduling, while pending age remains overdue. */
    assert(efs_query_refresh_due(52000,1000,50000,10000,0,0,256,2000));
    /* A slow completed build has already used the interval; no added delay. */
    assert(efs_query_refresh_due(5000,1001,1000,256,0,0,256,2000));
    int valid;
    assert(efs_query_export_lifetime(NULL,&valid)==3600000 && valid);
    assert(efs_query_export_lifetime("86400000",&valid)==86400000 && valid);
    assert(efs_query_export_lifetime("604800000",&valid)==604800000 && valid);
    assert(efs_query_export_lifetime("60000",&valid)==60000 && valid);
    const char *bad[]={"59999","604800001","-1","18446744073709551616","86400000x"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(efs_query_export_lifetime(bad[i],&valid)==3600000 && !valid);
    assert(!efs_query_export_expired(3600001,3599999,0,86400000));
    assert(efs_query_export_expired(3600001,3599999,0,3600000));
    assert(efs_query_export_expired(3600001,3500000,0,86400000));
    assert(!efs_query_export_expired(60000,0,0,60000));
    assert(efs_query_export_expired(60001,60000,0,60000));
    puts("query refresh: busy coalescing, sparse freshness, startup and retry PASS");
    return 0;
}
