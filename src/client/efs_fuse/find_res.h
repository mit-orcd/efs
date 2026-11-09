#ifndef EFS_FUSE_FIND_RES_H
#define EFS_FUSE_FIND_RES_H

#include "fuse_internal.h"

/* find_res.c: the .find result cache (.efs/find/<term> query files).
 *
 * Owner: find_res.c alone. The table (EFS_FIND_RES_N slots of one rendered
 *   server result each: directory, term, caller uid and gid, publisher
 *   hint, bytes), its mutex and its byte count (64 MiB in all, oldest
 *   evicted) are private to it; callers use only the operations below and
 *   never lock the table or read a slot.
 * Lock: one private table mutex, never held across an RPC and never held
 *   when an operation returns. Under it an operation may call vq_intern,
 *   vq_version_matches and virtual_file_open (support.c), which take their
 *   own mutexes: the table mutex is taken first. At most two fetches run
 *   at once (a second private counter); a third answers -EBUSY.
 * Every operation fetches first: the server result is reauthorized with
 *   the caller's uid, gid and supplementary groups on every call (no
 *   bytes are served from the table without that fetch), stored in the
 *   slot of its directory and term, then looked up again under the mutex
 *   by directory, term and the caller's uid and gid. Between the two the
 *   slot may have been replaced or evicted by another fetch: a miss.
 * Lifetime: a slot's bytes belong to the slot until a fetch replaces or
 *   evicts it or find_res_clear frees it at unmount. An open query file
 *   owns a copy (virtual_file_open); READ serves only that copy.
 * Answers (negative errno for fuse_reply_err):
 *   find_res_intern     0 and *q = the interned query node (reserved: the
 *                       caller vq_unreserve()s it); the fetch's error;
 *                       -EAGAIN on a miss; -ENOMEM when no node is free.
 *   find_res_open       0 with fi holding a copy of the bytes; the fetch's
 *                       error; -EAGAIN on a miss; -ESTALE when the bytes no
 *                       longer match the version LOOKUP interned;
 *                       -ENOMEM from virtual_file_open.
 *   find_query_fill_stat 0 with the result's length as the size; the
 *                       fetch's error; -1 on a miss.
 *   find_query_read     bytes copied, 0 at EOF or on a miss, or the fetch's
 *                       error. Reached only without a file handle.
 *   find_res_clear      frees every slot (efs_fuse_destroy).
 *   Fetch errors: the server rc through efs_rc_to_errno (BUSY stays EBUSY,
 *   an I/O failure EIO, a refusal EACCES), -EINVAL for an unparsable term,
 *   -EOVERFLOW, -ENAMETOOLONG, -EILSEQ unchanged, -EBUSY at the fetch
 *   limit, -ENOMEM when the byte budget cannot be met. */

int find_res_intern(efs_ino_t dir_ino, const char *query_path,
                    const char *term, fuse_ino_t *q);
int find_res_open(struct fuse_file_info *fi, fuse_ino_t ino, efs_ino_t dir_ino,
                  const char *query_path, const char *term);
int find_query_read(efs_ino_t dir_ino, const char *query_path,
                    const char *pattern, char *buf, size_t size, off_t offset);
int find_query_fill_stat(const struct efs_inode *parent,
                         const char *query_path, const char *pattern,
                         struct stat *stbuf);
void find_res_clear(void);

#endif
