#ifndef EFS_FUSE_GETATTR_H
#define EFS_FUSE_GETATTR_H

#include "fuse_internal.h"

/* getattr.c: LOOKUP memo, GETATTR and READDIR collection.
 *
 * Owns
 *   the lookup memo    LOOKUP_MEMO_N rows for LOOKUP_MEMO_US (50 ms), one
 *                      shot: the GETATTR that follows a LOOKUP consumes the
 *                      row instead of an RPC. Private mutex. A local
 *                      mutation (lookup_memo_mutation_begin/end, called by
 *                      ll_run.c and writeback.c) invalidates it and
 *                      suppresses it while in flight.
 *   struct efs_dirh    one directory open: fi->fh of OPENDIR (dirh_new),
 *                      filled by the first READDIR (dirh_fill), reused by
 *                      later READDIRs, freed at RELEASEDIR (dirh_free). One
 *                      FUSE request at a time touches it; no lock.
 *
 * Thread-locals (valid only inside the handler that set them; never kept
 * across an RPC that may block)
 *   t_lookup_memo_serial   ll_lookup (ll_run.c) sets it from
 *                      lookup_memo_start() before the row RPC and clears it
 *                      after the reply; 0 means the memo is suppressed. A
 *                      row is only memoized under the serial it was
 *                      fetched with, so an older reply cannot reinsert a
 *                      pre-mutation row (0j).
 *   t_getattr_row, t_getattr_ino
 *                      the row the current GETATTR fetched from the
 *                      server; lookup_fill (lookup.c) memoizes it when the
 *                      inode matches.
 *
 * Failure behaviour
 *   efs_fuse_getattr_ino, efs_fuse_access_ino and dirh_fill return a
 *   negative errno; an authority or availability failure keeps its own
 *   errno (EBUSY, EAGAIN, ESTALE, EIO), never ENOENT (I9). A failed
 *   dirh_fill leaves the listing empty and unlisted, so the next READDIR
 *   retries. */

struct readdir_ent {
    char name[EFS_MAX_NAME];
    struct stat st;
};

struct readdir_collect_arg {
    struct readdir_ent *ents;
    size_t count;
    size_t cap;
    size_t *name_index; /* Entry index plus one; zero is an empty bucket. */
    size_t name_index_cap;
};

/* Per-opendir listing. fi->fh is this pointer (files carry an open-description handle).
 * First READDIR fills it; later READDIR / EOF getdents reuse it. */
struct efs_dirh {
    efs_ino_t ino;
    struct readdir_ent *ents;
    size_t count;
    fuse_ino_t parent_ino;
    mode_t self_mode;
    mode_t parent_mode;
    int listed;
};

extern __thread uint64_t t_lookup_memo_serial;
extern __thread struct efs_inode t_getattr_row;
extern __thread efs_ino_t t_getattr_ino;
uint64_t fuse_now_us(void);
uint64_t lookup_memo_start(void);
void lookup_memo_mutation_begin(void);
void lookup_memo_mutation_end(void);
void lookup_memo_put(const struct efs_inode *row);
int efs_fuse_getattr_ino(fuse_ino_t ino, struct stat *stbuf,
                                struct fuse_file_info *fi);
struct efs_dirh *dirh_new(efs_ino_t ino);
void dirh_free(struct efs_dirh *dh);
int dirh_fill(struct efs_dirh *dh, fuse_ino_t ino);
int efs_fuse_access_ino(fuse_ino_t ino, int mask);

#endif
