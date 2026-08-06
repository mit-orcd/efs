#ifndef EFS_METADATA_H
#define EFS_METADATA_H

#include "efs/common.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

struct efs_chunk_entry {
    efs_ino_t ino;
    uint32_t chunk_index;
    efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
};

struct efs_inode {
    efs_ino_t ino;
    efs_ino_t parent;
    uint32_t mode;
    uid_t uid;
    gid_t gid;
    uint64_t size;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t nlink;
    char name[EFS_MAX_NAME];
};

struct efs_export {
    efs_export_id_t id;
    char name[EFS_MAX_NAME];
    uint64_t next_ino;
    struct efs_inode *inodes;
    uint64_t inode_count;
    uint64_t inode_capacity;
    struct efs_chunk_entry *chunks;
    uint64_t chunk_count;
    uint64_t chunk_capacity;

    /* Open-addressing indexes (key 0 = empty). Kept in sync on
     * create/unlink/rename/set_chunk so lookups stay O(1) at scale. */
    uint64_t *ino_keys;
    uint64_t *ino_vals;
    uint64_t ino_mask;
    uint64_t *name_keys;
    uint64_t *name_vals;
    uint64_t name_mask;
    uint64_t *chunk_keys;
    uint64_t *chunk_vals;
    uint64_t chunk_mask;
};

/* Initialize an empty export. */
void efs_export_init(struct efs_export *ex, efs_export_id_t id, const char *name);

/* Free an export. */
void efs_export_free(struct efs_export *ex);

/* Find a child inode by name under a parent. Returns 0 if found. */
int efs_export_lookup(struct efs_export *ex, efs_ino_t parent,
                      const char *name, struct efs_inode *out);

/* Find an inode by inode number. Returns 0 if found. */
int efs_export_get_inode(struct efs_export *ex, efs_ino_t ino,
                         struct efs_inode *out);

/* Create a new inode, allocating the next sequential inode number.
 * Returns the inode number or 0 on error. */
efs_ino_t efs_export_create(struct efs_export *ex, efs_ino_t parent,
                              uint32_t mode, uid_t uid, gid_t gid, const char *name);

/* Create a new inode with an explicit inode number (used by clients that
 * allocate from their own namespace so concurrent clients never collide).
 * Returns the inode number or 0 on error (including if the ino is taken). */
efs_ino_t efs_export_create_with_ino(struct efs_export *ex, efs_ino_t ino_num,
                                     efs_ino_t parent, uint32_t mode,
                                     uid_t uid, gid_t gid, const char *name);

/* Remove an inode and all of its chunk entries. */
int efs_export_unlink(struct efs_export *ex, efs_ino_t ino);

/* Set inode size. */
int efs_export_set_size(struct efs_export *ex, efs_ino_t ino, uint64_t size);

/* Set inode mode bits, preserving the file type. */
int efs_export_set_mode(struct efs_export *ex, efs_ino_t ino, uint32_t mode);

/* Set inode owner/group. Use (uid_t)-1 or (gid_t)-1 to leave unchanged. */
int efs_export_set_owner(struct efs_export *ex, efs_ino_t ino, uid_t uid, gid_t gid);

/* Set inode modification time. */
int efs_export_set_mtime(struct efs_export *ex, efs_ino_t ino, uint64_t mtime);

/* Rename/move an inode. If a destination inode already exists, it is replaced
   only when the source and destination are both regular files or both empty
   directories. Returns EFS_ERR_NOT_FOUND if the source does not exist. */
int efs_export_rename(struct efs_export *ex, efs_ino_t ino,
                      efs_ino_t new_parent, const char *new_name);

/* Add or update a chunk entry. */
int efs_export_set_chunk(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         const efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS],
                         const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE]);

/* Get a chunk entry. Returns 0 if found. */
int efs_export_get_chunk(struct efs_export *ex, efs_ino_t ino, uint32_t chunk_index,
                         struct efs_chunk_entry *out);

/* Serialize export metadata to a memory buffer. Caller must free *buf. */
int efs_export_serialize(struct efs_export *ex, char **buf, size_t *len);

/* Deserialize export metadata, replacing current contents. */
int efs_export_deserialize(struct efs_export *ex, const char *buf, size_t len);

/* Merge a client's metadata update into the server's committed export.
 * For each inode and chunk entry in `inc`, the server's copy is updated when
 * the incoming entry is newer (higher mtime for inodes) or not yet present.
 * This lets concurrent clients commit without dropping each other's data. */
int efs_export_merge(struct efs_export *ex, const struct efs_export *inc);

/* Load from / save to a file. */
int efs_export_load(struct efs_export *ex, const char *path);
int efs_export_save(struct efs_export *ex, const char *path);

#endif
