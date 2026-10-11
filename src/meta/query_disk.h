#ifndef EFS_QUERY_DISK_H
#define EFS_QUERY_DISK_H
#include "efs/query_catalog.h"
struct efs_query_disk;
int efs_query_disk_build(const char *,struct efs_kv_lsm_snapshot *,
    const struct efs_query_disk *,uint64_t,int (*)(void *),void *,struct efs_query_build_times *,struct efs_query_disk **);
int efs_query_disk_build_checked(const char *,struct efs_kv_lsm_snapshot *,
    const struct efs_query_disk *,uint64_t,int (*)(void *),void *,struct efs_query_build_times *,
    struct efs_query_build_failure *,struct efs_query_disk **);
void efs_query_disk_free(struct efs_query_disk *);
uint64_t efs_query_disk_bytes(const struct efs_query_disk *);
uint32_t efs_query_disk_refolded(const struct efs_query_disk *);
uint32_t efs_query_disk_rebuilt(const struct efs_query_disk *);
int efs_query_disk_inode(const struct efs_query_disk *,efs_ino_t,struct efs_meta_row *,struct efs_meta_stat *);
int efs_query_disk_stats(const struct efs_query_disk *,efs_ino_t,struct efs_query_rollup *);
int efs_query_disk_link(const struct efs_query_disk *,efs_ino_t,struct efs_name_entry *);
int efs_query_disk_path(const struct efs_query_disk *,efs_ino_t,efs_ino_t,char *,size_t);
int efs_query_disk_page(const struct efs_query_disk *,efs_ino_t,enum efs_name_match,const char *,int,
    uint64_t *,uint32_t,int (*)(void *),void *,int (*)(void *,const struct efs_name_entry *,const char *),void *);
int efs_query_disk_ls(const struct efs_query_disk *,efs_ino_t,uint64_t,int (*)(void *),void *,
    int (*)(void *,const struct efs_name_entry *,const char *),void *);
int efs_query_disk_checkpoint(struct efs_query_disk *,const uint64_t[4]);
int efs_query_disk_rebase(struct efs_query_disk *,struct efs_kv_lsm_snapshot *);
int efs_query_disk_restore(const char *,const uint64_t[4],uint64_t,struct efs_query_disk **);
#endif
