#ifndef EFS_STORAGE_ROOTS_H
#define EFS_STORAGE_ROOTS_H
#include "server_internal.h"
/* Storage roots: locks, root marker, ancestry barriers.
 * Root format 1 = every fragment file carries the I25 identity-bound record
 * (efs/frag_record.h, fragment format 2). Root format 0 held payload-only
 * digest tails and is refused, never read. */
#define EFS_STORAGE_ROOT_FORMAT 1u
int server_storage_roots_init(struct efsd_server *s);
void server_storage_roots_close(struct efsd_server *s);
int server_storage_sync_parents_with(struct efsd_server *s, const char *path,
                                    int (*sync)(void *,int), void *context);
/* Nonzero when a root-relative path is a metadata-table fragment path. */
int server_storage_metadata_path(const char *relative);
/* 0 unmarked, EFS_STORAGE_ROOT_FORMAT, or -1: unreadable, corrupt or
 * another (refused) root format. */
int server_storage_root_mode(const char *root);
#endif
