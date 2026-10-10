#include "../src/server/query_refresh.h"
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
    /* A slow completed build has already used the interval; no added delay. */
    assert(efs_query_refresh_due(5000,1001,1000,256,0,0,256,2000));
    puts("query refresh: busy coalescing, sparse freshness, startup and retry PASS");
    return 0;
}
