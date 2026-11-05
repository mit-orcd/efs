#ifndef EFS_FUSE_LOOKUP_H
#define EFS_FUSE_LOOKUP_H

#include "fuse_internal.h"

/* lookup.c: LOOKUP, SETATTR and deep-callback sampling.
 *
 * Owns no state beyond the sampler. fill_entry* render a fuse_entry_param
 * with attr_timeout 0 (the kernel revalidates every attribute); lookup_fill
 * memoizes the row the current GETATTR fetched (getattr.h thread-locals).
 * struct deep_callback_sample brackets one handler
 * (deep_callback_begin / reply_begin / end) to log slow callbacks; it is a
 * value on the handler's stack, never shared.
 *
 * Failure behaviour: efs_fuse_lookup_at returns -ENOENT only for an absent
 * name; a committed-reply wait that times out or an authority failure
 * keeps its errno (EBUSY, EAGAIN, ESTALE, EIO), per I9. SETATTR dispatches
 * to namespace.h and returns its errno. */

struct deep_callback_sample {
    uint64_t begin, service_end;
    long pid;
    char name[6];
};

void fill_entry(fuse_ino_t ino, struct fuse_entry_param *e,
                       const struct stat *st);
void fill_entry_row(fuse_ino_t ino, struct fuse_entry_param *e,
                           const struct efs_inode *row);
int lookup_fill(fuse_ino_t ino, struct fuse_entry_param *e,
                       struct fuse_file_info *fi);
int lookup_fill_committed(fuse_ino_t ino, struct fuse_entry_param *e,
                                 const char *op, fuse_ino_t parent,
                                 const char *name);
int efs_fuse_lookup_at(fuse_ino_t parent, const char *name,
                              struct fuse_entry_param *e);
int efs_fuse_setattr_ino(fuse_ino_t ino, struct stat *attr, int to_set,
                                struct fuse_file_info *fi);
int dirbuf_add(fuse_req_t req, char *buf, size_t bufsize, size_t *used,
                      const char *name, fuse_ino_t ino, mode_t mode, off_t next);
struct deep_callback_sample deep_callback_begin(fuse_req_t req, const char *name);
void deep_callback_reply_begin(struct deep_callback_sample *sample);
void deep_callback_end(struct deep_callback_sample sample, const char *op,
                              fuse_ino_t parent, int rc);

#endif
