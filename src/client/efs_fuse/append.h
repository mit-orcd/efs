#ifndef EFS_FUSE_APPEND_H
#define EFS_FUSE_APPEND_H

#include "fuse_internal.h"

/* append.c: O_APPEND reservation and publication, the pressure flush.
 *
 * Owns
 *   append_mu(ino)     APPEND_MU_N stripes serializing same-inode append
 *                      writes with close/fsync flush and the REPORT that
 *                      publishes it; private, initialized once.
 *   reservation        append_reserve_offset asks the metadata owner for
 *                      the region (never a local end); the row's
 *                      append_burst* fields in struct efs_open_ref (open.h)
 *                      are read and updated under g_open_mu.
 *   the pressure worker  fuse_pressure_kick starts it, fuse_pressure_stop
 *                      joins it at destroy; it publishes a fixed dirty
 *                      cohort per round and never consumes the foreground
 *                      write-back error (g_wb.err, writeback.h).
 *
 * Failure behaviour
 *   append_reserve_offset: the caller holds the stripe; success keeps it,
 *   every failure releases it. EFS_ERR_BUSY when another client still has
 *   OPEN reservations on the inode or the deadline passed. -EAGAIN when
 *   no reservation slot is free.
 *   efs_append_flush_report and fuse_pressure_flush run under one
 *   deadline that their worker RPCs inherit; they return the EFS_ERR_* of
 *   the first failed PUT or REPORT, EFS_ERR_BUSY on the deadline, and
 *   write.c maps that to the caller's errno. */

pthread_mutex_t *append_mu(efs_ino_t ino);
int efs_append_flush_report(struct fuse_file_info *fi, efs_ino_t ino);
int fuse_pressure_flush(efs_ino_t ino);
int fuse_pressure_kick(void);
void fuse_pressure_stop(void);
int efs_utimens_flush_dirty(efs_ino_t ino);
off_t append_end_offset(efs_ino_t ino);
int append_reserve_offset(efs_ino_t ino, uint64_t len, off_t *off_out);
int append_publish_burst(efs_ino_t ino);

#endif
