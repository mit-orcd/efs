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
    uint32_t mtime_nsec; /* nanoseconds portion of mtime (for rsync etc.) */
    uint64_t ctime;
    uint32_t nlink;
    char name[EFS_MAX_NAME];
};

/* Tiny fully-replicated export root. Bulk inode/chunk tables live in
 * 2+1 metadata pages under EFS_META_TABLE_INO (see efs_meta_page_*).
 * page_checksums is heap-allocated: page_count * EFS_NUM_FRAGMENTS * HASH. */
struct efs_export_root {
    uint32_t version;
    efs_export_id_t id;
    char name[EFS_MAX_NAME];
    uint64_t next_ino;
    uint64_t generation;
    uint32_t blob_len;   /* length of the EFSM blob packed into pages */
    uint32_t page_count; /* ceil(blob_len / EFS_CHUNK_SIZE) */
    uint8_t *page_checksums;
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

    /* When set, metadata.bin stores efs_export_root (EFSR); bulk tables are
     * reconstructed from 2+1 pages. The in-memory inode/chunk arrays remain
     * the working cache after rebuild. */
    int meta_fragmented;
    /* Set when root advanced but inode/chunk tables not yet rebuilt from pages.
     * Cleared by server_rebuild_export_from_pages. */
    int meta_needs_rebuild;
    struct efs_export_root root;
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

/* Remove one directory name. If it was the last hard link, also remove chunks. */
int efs_export_unlink_name(struct efs_export *ex, efs_ino_t parent, const char *name);

/* Add a hard link (extra name) for an existing non-directory inode. */
int efs_export_link(struct efs_export *ex, efs_ino_t src_ino,
                    efs_ino_t new_parent, const char *new_name);

/* Set inode size. */
int efs_export_set_size(struct efs_export *ex, efs_ino_t ino, uint64_t size);

/* Drop chunk map entries with chunk_index >= first_chunk (truncate shrink). */
void efs_export_drop_chunks_from(struct efs_export *ex, efs_ino_t ino,
                                 uint32_t first_chunk);

/* Set inode mode bits, preserving the file type. */
int efs_export_set_mode(struct efs_export *ex, efs_ino_t ino, uint32_t mode);

/* Set inode owner/group. Use (uid_t)-1 or (gid_t)-1 to leave unchanged. */
int efs_export_set_owner(struct efs_export *ex, efs_ino_t ino, uid_t uid, gid_t gid);

/* Set inode modification time (seconds; clears nanoseconds). */
int efs_export_set_mtime(struct efs_export *ex, efs_ino_t ino, uint64_t mtime);

/* Set inode modification time with nanosecond precision. */
int efs_export_set_mtime_ns(struct efs_export *ex, efs_ino_t ino,
                            uint64_t mtime, uint32_t mtime_nsec);

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

/* Load from / save to a file. Fragmented exports save/load EFSR roots. */
int efs_export_load(struct efs_export *ex, const char *path);
int efs_export_save(struct efs_export *ex, const char *path);

/* --- Fragmented metadata (hybrid root + 2+1 pages) --- */

int efs_meta_blob_is_root(const char *buf, size_t len);
int efs_meta_blob_is_export(const char *buf, size_t len);

uint32_t efs_meta_page_count_for_blob(uint32_t blob_len);

/* Copy one zero-padded EFS_CHUNK_SIZE page out of a serialized EFSM blob. */
int efs_meta_extract_page(const char *blob, uint32_t blob_len, uint32_t page_index,
                          uint8_t page_out[EFS_CHUNK_SIZE]);

/* Assemble pages back into a blob of root->blob_len bytes. */
int efs_meta_assemble_blob(const struct efs_export_root *root,
                           const uint8_t pages[][EFS_CHUNK_SIZE],
                           char **blob_out, size_t *blob_len_out);

int efs_export_root_serialize(const struct efs_export_root *root,
                              char **buf, size_t *len);
int efs_export_root_deserialize(struct efs_export_root *root,
                                const char *buf, size_t len);

/* Fill root header from export (allocates page_checksums for page_count). */
int efs_export_root_prepare(struct efs_export_root *root,
                            const struct efs_export *ex,
                            uint64_t generation,
                            uint32_t blob_len);

/* Free page_checksums; safe on zeroed roots. */
void efs_export_root_free(struct efs_export_root *root);

/* Move root contents into dst (steals page_checksums; clears src). */
void efs_export_root_move(struct efs_export_root *dst, struct efs_export_root *src);

/* Deep-copy root (including checksums). */
int efs_export_root_copy(struct efs_export_root *dst, const struct efs_export_root *src);

/* Pointer to checksums[page][frag] inside root->page_checksums. */
static inline uint8_t *efs_export_root_checksum(struct efs_export_root *root,
                                                uint32_t page, int frag)
{
    return root->page_checksums +
           ((size_t)page * EFS_NUM_FRAGMENTS + (size_t)frag) * EFS_HASH_SIZE;
}

static inline const uint8_t *efs_export_root_checksum_const(
    const struct efs_export_root *root, uint32_t page, int frag)
{
    return root->page_checksums +
           ((size_t)page * EFS_NUM_FRAGMENTS + (size_t)frag) * EFS_HASH_SIZE;
}

#endif
