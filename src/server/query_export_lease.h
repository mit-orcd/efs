#ifndef EFS_QUERY_EXPORT_LEASE_H
#define EFS_QUERY_EXPORT_LEASE_H
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
/* Finite total lease for large streaming exports; idle expiry is separate. */
static inline uint64_t efs_query_export_lifetime(const char *text,int *valid)
{
    *valid=1;
    if(!text || !*text)return UINT64_C(3600000);
    char *end=NULL;errno=0;unsigned long long value=strtoull(text,&end,10);
    if(errno || !end || *end || value<60000 || value>604800000){
        *valid=0;return UINT64_C(3600000);
    }
    return (uint64_t)value;
}
static inline int efs_query_export_expired(uint64_t now,uint64_t touched,
                                           uint64_t born,uint64_t lifetime)
{
    return now-touched>60000 || now-born>lifetime;
}
#endif
