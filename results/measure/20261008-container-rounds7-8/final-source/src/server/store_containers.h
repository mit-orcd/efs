#ifndef EFS_STORE_CONTAINERS_H
#define EFS_STORE_CONTAINERS_H
#include "server_internal.h"
/* Experimental physical backend. Caller holds inode/fragment lifetime locks. */
int server_storage_engine_init(struct efsd_server *s);
int server_storage_files_attach(struct efsd_server *s, const char *root, unsigned index);
void server_storage_engine_close(struct efsd_server *s);
int server_container_metadata_path(const char *relative);
int server_storage_root_mode(const char *root);
int server_container_get(struct efsd_server *s, const char *path, uint8_t *data,
                         uint32_t *len, uint8_t *sum, int *sum_ok);
int server_container_info(struct efsd_server *s, const char *path,
                          uint32_t *len, uint8_t *sum);
int server_container_payload(const char *path, uint64_t *len);
/* Reserves/reconciles logical quota using the same mapping as retirement. */
int server_container_put(struct efsd_server *s, const char *path, const uint8_t *data,
                         uint32_t len, const uint8_t *sum, int *published);
int server_container_delete(struct efsd_server *s, const char *path,
                            const uint8_t *expect, uint64_t *payload, uint64_t *bytes);
#endif
