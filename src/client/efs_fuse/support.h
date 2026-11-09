#ifndef EFS_FUSE_SUPPORT_H
#define EFS_FUSE_SUPPORT_H

#include "fuse_internal.h"

/* support.c: process-wide helpers (crash handlers, .stats text, feature
 * bits, file handles, virtual inodes, invalidation, DAC). The .find result
 * cache is find_res.c behind find_res.h.
 *
 * Owns
 *   struct stats_ent   one directory's rendered .stats text; filled by
 *                      stats_snapshot (one RPC each, no cache in this file).
 *   feature_enabled    export feature bits, cached for FEATURES_TTL_MS under
 *                      a private mutex and re-read from the server after.
 *   efs_file_open/close, virtual_file_open
 *                      fi->fh of a file open is a tagged struct
 *                      efs_file_handle (fuse_internal.h), allocated at OPEN
 *                      and freed at RELEASE; a virtual open owns its
 *                      immutable snapshot bytes.
 *   virt_*, vq_*       the .stats/.find/.efs name space in the high ino
 *                      bits; EFS_VQ_N interned query nodes, pinned by the
 *                      kernel's nlookup (vq_nlookup_*), private mutex.
 *   ll_inval_*         the invalidation thread; notify_inval_* is queued by
 *                      the ll_* wrapper after fuse_reply_*, never run inline.
 *   check_*, fill_stat_from_inode, efs_create_credentials
 *                      DAC and stat rendering, pure functions of the row
 *                      and the caller context (ll_ctx reads t_req).
 *
 * Shared state and locks
 *   g_fuse_se          written by main() before fuse_session_loop_mt and
 *                      after it returns; the invalidation thread reads it
 *                      only between ll_inval_start and ll_inval_stop. No lock.
 *
 * Failure behaviour
 *   Helpers return a negative errno ready for fuse_reply_err. A server rc
 *   is mapped by fuse_stat_errno / efs_rc_to_errno (fuse_internal.h): BUSY
 *   and a passed deadline stay EBUSY/EAGAIN, an I/O failure is EIO, and
 *   ENOENT is only ever an absent name (I9). query_credentials fills a
 *   query's uid, gid and supplementary groups (EFS_ERR_OVERFLOW past
 *   EFS_QUERY_GROUPS). efs_file_sync_error
 *   folds two error channels for FSYNC/FLUSH: the per-description error
 *   retained since the failed PUT (h->errors) and a still-stalled inode
 *   (efs_wb_inode_stalled, ../write.c); either is EFS_ERR_IO. */

#define EFS_STATS_TEXT   2048

struct stats_ent {
    int valid;
    efs_ino_t dir_ino;
    uint64_t computed_ms; /* CLOCK_MONOTONIC ms when computed */
    uint64_t uid, gid;
    uint64_t mtime, atime, ctime;
    int len;
    char text[EFS_STATS_TEXT];
};

void efs_fuse_install_crash_handlers(void);
uint32_t fuse_chunk_size(void);
void stat_set_size_blocks(struct stat *stbuf, uint64_t size,
                                 uint64_t allocated);
int stats_snapshot(efs_ino_t dir_ino,struct stats_ent *out);
int stats_fill_stat(const struct efs_inode *parent, struct stat *stbuf);
int feature_enabled(uint32_t bit);
extern struct fuse_session *g_fuse_se;
int efs_file_open(struct fuse_file_info *fi, efs_ino_t ino);
void efs_file_close(struct fuse_file_info *fi);
int virtual_file_open(struct fuse_file_info *fi, efs_ino_t ino,
                             const char *text, size_t len);
int efs_file_sync_error(struct fuse_file_info *fi, efs_ino_t ino,
                                int was_stalled, int rc);
efs_ino_t virt_parent(fuse_ino_t ino);
fuse_ino_t virt_find_ino(efs_ino_t parent);
fuse_ino_t virt_commands_ino(efs_ino_t parent);
int commands_enabled(void);
int command_script(const char *term);
uint64_t virtual_version(const char *text, size_t len);
fuse_ino_t vq_intern(efs_ino_t dir_ino, const char *term,
                            uint64_t version, size_t len);
void vq_unreserve(fuse_ino_t nodeid);
int vq_lookup(fuse_ino_t nodeid, efs_ino_t *dir_out, char *term_out,
                     size_t term_cap);
void vq_nlookup_add(fuse_ino_t nodeid, uint32_t n);
void vq_nlookup_sub(fuse_ino_t nodeid, uint64_t n);
int vq_pinned_size(efs_ino_t ino, size_t *len, int open_only);
int vq_version_matches(efs_ino_t ino, const char *text, size_t len);
const struct fuse_ctx *ll_ctx(void);
void efs_create_credentials(const struct efs_inode *parent, mode_t *mode,
                                   uid_t *uid, gid_t *gid);
void ll_inval_entry(fuse_ino_t parent, const char *name);
void ll_inval_inode(fuse_ino_t ino, off_t off, off_t len);
int ll_inval_start(void);
void ll_inval_stop(void);
int virtual_access(efs_ino_t ino);
int query_credentials(struct efs_msg_query_snapshot *q);
int find_dir_fill_stat(const struct efs_inode *parent, struct stat *stbuf);
int check_access(const struct efs_inode *ino, uid_t uid, gid_t gid,
                        int mask);
int check_dir_wx(const struct efs_inode *dir);
int check_dir_x(const struct efs_inode *dir);
void fill_stat_from_inode(struct stat *stbuf, const struct efs_inode *ino);
int check_chown_perm(const struct efs_inode *ino, uid_t uid, gid_t gid);
int file_chunk_present(efs_ino_t ino, uint32_t ci);

#endif
