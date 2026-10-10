#ifndef EFS_QUERY_CATALOG_H
#define EFS_QUERY_CATALOG_H
#include "efs/name_index.h"

struct efs_query_catalog;
struct efs_query_rollup {
    uint64_t imm_files, imm_dirs, imm_bytes, imm_tmin, imm_tmax;
    uint64_t tree_files, tree_dirs, tree_bytes, tree_tmin, tree_tmax;
};
/* Background-only builder. Reuses unchanged immutable partitions from old;
 * folds all directory aggregates in RAM. Source must include EVERY metadata
 * group, at a transaction-safe cut established by the host. Local pending
 * records also refuse publication. Limits cover accounted allocations of one
 * candidate catalog; host must separately bound concurrent published versions.
 * A failed build leaves old untouched. Keep old alive throughout this call. A retained catalog may outlive publication; atomic references protect shared
 * partitions and contributions. Borrowed readers must retain or hold the host
 * publication lock.
 * stop may cancel long builds and must be cheap/nonblocking. */
int efs_query_catalog_build(struct efs_kv_lsm_snapshot *source,
                             const struct efs_query_catalog *old,
                             uint64_t max_bytes, int (*stop)(void *), void *ctx,
                             struct efs_query_catalog **out);
/* Persistent local backend. root is a private cache directory on metadata
 * storage. Same authoritative source/retention contract as the RAM builder;
 * allocations remain bounded independently of namespace cardinality. */
int efs_query_catalog_build_disk(const char *root,struct efs_kv_lsm_snapshot *source,
    const struct efs_query_catalog *old,uint64_t max_bytes,int (*stop)(void *),void *ctx,
    struct efs_query_catalog **out);
/* Checkpoint/restore require identical applied indexes and terms for both
 * metadata groups, supplied only after the host establishes authority. */
int efs_query_catalog_checkpoint(struct efs_query_catalog *,const uint64_t watermark[4]);
/* Refresh a stale completed checkpoint; never publishes its stale contents. */
int efs_query_catalog_catchup_disk(const char *root,struct efs_kv_lsm_snapshot *source,
    uint64_t max_bytes,int (*stop)(void *),void *ctx,struct efs_query_catalog **out);
int efs_query_catalog_restore_disk(const char *,struct efs_kv_lsm_snapshot *,const uint64_t watermark[4],uint64_t,
    struct efs_query_catalog **);
void efs_query_catalog_retain(struct efs_query_catalog *catalog);
/* Resume a name-index scan. stop/emit AGAIN retains the current candidate;
 * BUSY from the scan budget becomes a successful page with a continuation. */
int efs_query_catalog_page(const struct efs_query_catalog *catalog,efs_ino_t root,
    enum efs_name_match match,const char *term,int immediate,uint64_t *cursor,
    uint32_t budget,int (*stop)(void *),void *stop_ctx,
    int (*emit)(void *,const struct efs_name_entry *,const char *),void *user);
void efs_query_catalog_free(struct efs_query_catalog *catalog);
uint64_t efs_query_catalog_bytes(const struct efs_query_catalog *catalog);
/* Per-build work counters; dependency reuse includes inode and active lane
 * partitions and assumes the same transaction-safe source contract above. */
uint32_t efs_query_catalog_refolded(const struct efs_query_catalog *catalog);
uint32_t efs_query_catalog_reused_edges(const struct efs_query_catalog *catalog);
uint32_t efs_query_catalog_rebuilt(const struct efs_query_catalog *catalog);
int efs_query_catalog_inode(const struct efs_query_catalog *catalog, efs_ino_t ino,
                             struct efs_meta_row *row, struct efs_meta_stat *stat);
int efs_query_catalog_stats(const struct efs_query_catalog *catalog, efs_ino_t ino,
                             struct efs_query_rollup *out);
/* Copy the chosen directory edge for fresh ancestry/name validation. Root
 * and detached directories have no edge and return NOT_FOUND. */
int efs_query_catalog_directory_link(const struct efs_query_catalog *catalog,
                                       efs_ino_t ino,struct efs_name_entry *out);
/* Source-relative directory path, no mount prefix; root gives empty string.
 * NOT_FOUND means outside root; NAMETOOLONG and cycles fail explicitly. */
int efs_query_catalog_path(const struct efs_query_catalog *catalog,
                            efs_ino_t dir, efs_ino_t root, char *out, size_t cap);
/* Name candidates are checked against the catalog's authoritative directory
 * layout and generation before emission. Host must ALSO validate fresh user
 * permissions, ancestry and query lifetime before returning any bytes. */
int efs_query_catalog_find(const struct efs_query_catalog *catalog,
                            efs_ino_t root, enum efs_name_match match,
                            const char *term, uint64_t visit_budget,
                            int (*emit)(void *, const struct efs_name_entry *,
                                        const char *relative), void *user);
/* Immediate entries from the immutable catalog's directory lanes, not a full
 * name-index scan or any live tree walk. Budget limits scanned records, including tombstones. */
int efs_query_catalog_ls(const struct efs_query_catalog *catalog, efs_ino_t root,
                          uint64_t visit_budget,
                          int (*emit)(void *, const struct efs_name_entry *, const char *), void *user);
/* Cancellation is checked for scanned records even when no name is emitted. */
int efs_query_catalog_ls_bounded(const struct efs_query_catalog *catalog,efs_ino_t root,
                          uint64_t visit_budget,int (*stop)(void *),void *stop_ctx,
                          int (*emit)(void *,const struct efs_name_entry *,const char *),void *user);
/* Startup-only orphan recovery; hold returned descriptor until worker exit. */
int efs_query_catalog_disk_recover(const char *root,int *lease);
#endif
