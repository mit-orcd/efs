#ifndef EFS_QUERY_FILTERS_H
#define EFS_QUERY_FILTERS_H
#include "efs/protocol.h"
#include <sys/stat.h>
static inline int efs_query_filters_valid(const struct efs_query_filters *f)
{
    return f && !(f->flags & ~15u) && f->kind<=3 &&
        ((f->flags&EFS_QUERY_MIN_SIZE) || !f->min_size) &&
        ((f->flags&EFS_QUERY_MAX_SIZE) || !f->max_size) &&
        ((f->flags&EFS_QUERY_AFTER) || !f->after_ns) &&
        ((f->flags&EFS_QUERY_BEFORE) || !f->before_ns) &&
        ((f->flags&3)!=3 || f->min_size<=f->max_size) &&
        ((f->flags&12)!=12 || f->after_ns<f->before_ns);
}
static inline int efs_query_filters_match(const struct efs_query_filters *f,
                                          uint32_t mode,uint64_t size,uint64_t mtime)
{
    if (f->kind && !(f->kind==1 ? S_ISREG(mode) : f->kind==2 ? S_ISDIR(mode) : S_ISLNK(mode))) return 0;
    return (!(f->flags&EFS_QUERY_MIN_SIZE) || size>=f->min_size) &&
           (!(f->flags&EFS_QUERY_MAX_SIZE) || size<=f->max_size) &&
           (!(f->flags&EFS_QUERY_AFTER) || mtime>f->after_ns) &&
           (!(f->flags&EFS_QUERY_BEFORE) || mtime<f->before_ns);
}
#endif
