#ifndef EFS_STORAGE_ROOTS_H
#define EFS_STORAGE_ROOTS_H
#include "server_internal.h"
/* Files-engine storage roots: locks, engine marker, ancestry barriers. */
int server_storage_roots_init(struct efsd_server *s);
int server_storage_files_attach(struct efsd_server *s, const char *root, unsigned index);
void server_storage_roots_close(struct efsd_server *s);
int server_storage_sync_parents(struct efsd_server *s, const char *path);
int server_storage_sync_parents_with(struct efsd_server *s, const char *path,
                                    int (*sync)(void *,int), void *context);
/* Nonzero when a root-relative path is a metadata-table fragment path. */
int server_storage_metadata_path(const char *relative);
/* 0 unmarked, engine byte from the root marker, -1 unreadable/corrupt. */
int server_storage_root_mode(const char *root);
#endif
