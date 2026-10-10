#ifndef EFS_QUERY_POLICY_H
#define EFS_QUERY_POLICY_H
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
/* Called from serialized Raft apply, never a concurrent read-modify-write. */
int efs_query_policy_get(struct efs_kv *, uint64_t *, uint32_t *);
int efs_query_policy_apply(struct efs_kv *, uint64_t, uint32_t);
#endif
