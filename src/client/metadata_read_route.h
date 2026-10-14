#ifndef EFS_METADATA_READ_ROUTE_H
#define EFS_METADATA_READ_ROUTE_H
#include "efs/common.h"
#include <pthread.h>
#include <string.h>

/* Bounded direct-mapped routing hints, no attributes, decisions or freshness
 * certificates. A collision/stale entry can only add a validated RPC hop.
 * The complete original request is always sent to the selected live peer. */
#define EFS_META_READ_ROUTES 128u
struct efs_meta_read_routes {
    pthread_mutex_t mu;
    struct { uint64_t key; efs_node_id_t node; } slot[EFS_META_READ_ROUTES];
};
static inline uint64_t efs_meta_read_route_key(uint8_t type,const void *req,uint32_t n)
{
    const uint8_t *p=req;
    uint64_t key=(14695981039346656037ull^type)*1099511628211ull;
    for(uint32_t i=0;i<n;i++)key=(key^p[i])*1099511628211ull;
    return key;
}
static inline efs_node_id_t efs_meta_read_route_get(struct efs_meta_read_routes *r,uint64_t key)
{
    unsigned i=(unsigned)key&(EFS_META_READ_ROUTES-1);
    pthread_mutex_lock(&r->mu);
    efs_node_id_t node=r->slot[i].key==key?r->slot[i].node:0;
    pthread_mutex_unlock(&r->mu);
    return node;
}
static inline void efs_meta_read_route_note(struct efs_meta_read_routes *r,uint64_t key,efs_node_id_t node)
{
    unsigned i=(unsigned)key&(EFS_META_READ_ROUTES-1);
    pthread_mutex_lock(&r->mu);
    r->slot[i].key=key;r->slot[i].node=node;
    pthread_mutex_unlock(&r->mu);
}
static inline void efs_meta_read_route_reset(struct efs_meta_read_routes *r)
{
    pthread_mutex_lock(&r->mu);
    memset(r->slot,0,sizeof(r->slot));
    pthread_mutex_unlock(&r->mu);
}
#endif
