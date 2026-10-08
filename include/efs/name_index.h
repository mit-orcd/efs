#ifndef EFS_NAME_INDEX_H
#define EFS_NAME_INDEX_H

#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"

/* Immutable shard-local candidate index, built by a background worker from
 * one full applied KV snapshot. Never build on a query/read path. This is not
 * a published cluster snapshot: callers must establish group authority and
 * transaction-safe snapshot assembly before serving users. Dentry tombstones
 * are retained; directory layout chooses hashed-vs-local precedence. Query
 * candidates still need directory scope, generation and permission validation.
 * A prepared intent/guard/reduction in the shard refuses a new build (BUSY).
 * A caller may retain an older published version with truthful freshness. */
struct efs_name_index;
struct efs_name_entry {
    efs_ino_t parent;
    struct efs_meta_dentry dentry;
    char name[EFS_MAX_NAME];
};
enum efs_name_match {
    EFS_NAME_EXACT, EFS_NAME_PREFIX, EFS_NAME_SUFFIX, EFS_NAME_CONTAINS
};
int efs_name_index_build(struct efs_kv_lsm_snapshot *source, uint32_t shard,
                          uint64_t max_bytes, struct efs_name_index **out);
void efs_name_index_free(struct efs_name_index *index);
uint64_t efs_name_index_revision(const struct efs_name_index *index);
uint32_t efs_name_index_count(const struct efs_name_index *index);
/* Cursor belongs to this immutable index and pattern; never reuse it across
 * versions or queries. At most visit_budget candidate rows are visited, and
 * *cursor advances after each accepted callback. BUSY is an explicit partial
 * page requiring continuation, not a complete successful result. Exact and
 * prefix queries seek their name range; suffix/contains have bounded scans.
 * Concurrent readers are allowed; join readers before freeing the index. */
int efs_name_index_query(const struct efs_name_index *index,
                          enum efs_name_match match, const char *term,
                          uint32_t *cursor, uint32_t visit_budget,
                          int (*emit)(void *, const struct efs_name_entry *),
                          void *user);

#endif
