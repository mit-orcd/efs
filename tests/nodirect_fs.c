/* A FUSE passthrough of one backing directory that refuses O_DIRECT opens
 * with EINVAL, as a filesystem without direct I/O support does. Test helper
 * for the D42 startup refusal (tests/test_direct_io_required.py); kernels
 * from 6.6 accept O_DIRECT on tmpfs, so tmpfs no longer serves.
 *
 * Usage: tests/nodirect_fs BACKING_DIR MOUNTPOINT [fuse options] */
#define FUSE_USE_VERSION 312
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fuse3/fuse.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static char backing[PATH_MAX];

static int real(const char *path, char out[PATH_MAX])
{
    return snprintf(out, PATH_MAX, "%s%s", backing, path) >= PATH_MAX ? -ENAMETOOLONG : 0;
}
#define REAL(p, r) char r[PATH_MAX]; { int e_ = real(p, r); if (e_) return e_; }

static int nd_getattr(const char *p, struct stat *st, struct fuse_file_info *fi)
{
    if (fi) return fstat((int)fi->fh, st) ? -errno : 0;
    REAL(p, r);
    return lstat(r, st) ? -errno : 0;
}
static int nd_readdir(const char *p, void *buf, fuse_fill_dir_t fill, off_t off,
                      struct fuse_file_info *fi, enum fuse_readdir_flags fl)
{
    (void)off; (void)fi; (void)fl;
    REAL(p, r);
    DIR *d = opendir(r);
    if (!d) return -errno;
    struct dirent *e;
    while ((e = readdir(d)))
        if (fill(buf, e->d_name, NULL, 0, 0)) break;
    closedir(d);
    return 0;
}
static int nd_mkdir(const char *p, mode_t m) { REAL(p, r); return mkdir(r, m) ? -errno : 0; }
static int nd_rmdir(const char *p) { REAL(p, r); return rmdir(r) ? -errno : 0; }
static int nd_unlink(const char *p) { REAL(p, r); return unlink(r) ? -errno : 0; }
static int nd_rename(const char *a, const char *b, unsigned int fl)
{
    if (fl) return -EINVAL;
    REAL(a, ra); REAL(b, rb);
    return rename(ra, rb) ? -errno : 0;
}
static int nd_truncate(const char *p, off_t n, struct fuse_file_info *fi)
{
    if (fi) return ftruncate((int)fi->fh, n) ? -errno : 0;
    REAL(p, r);
    return truncate(r, n) ? -errno : 0;
}
static int nd_open_flags(const char *p, int flags, mode_t m, struct fuse_file_info *fi)
{
    if (flags & O_DIRECT) return -EINVAL; /* no direct I/O on this filesystem */
    REAL(p, r);
    int fd = open(r, flags, m);
    if (fd < 0) return -errno;
    fi->fh = (uint64_t)fd;
    return 0;
}
static int nd_open(const char *p, struct fuse_file_info *fi) { return nd_open_flags(p, fi->flags, 0, fi); }
static int nd_create(const char *p, mode_t m, struct fuse_file_info *fi)
{
    return nd_open_flags(p, fi->flags | O_CREAT, m, fi);
}
static int nd_read(const char *p, char *b, size_t n, off_t o, struct fuse_file_info *fi)
{
    (void)p;
    ssize_t r = pread((int)fi->fh, b, n, o);
    return r < 0 ? -errno : (int)r;
}
static int nd_write(const char *p, const char *b, size_t n, off_t o, struct fuse_file_info *fi)
{
    (void)p;
    ssize_t r = pwrite((int)fi->fh, b, n, o);
    return r < 0 ? -errno : (int)r;
}
static int nd_fsync(const char *p, int ds, struct fuse_file_info *fi)
{
    (void)p;
    return (ds ? fdatasync((int)fi->fh) : fsync((int)fi->fh)) ? -errno : 0;
}
static int nd_release(const char *p, struct fuse_file_info *fi) { (void)p; close((int)fi->fh); return 0; }
static int nd_statfs(const char *p, struct statvfs *s) { REAL(p, r); return statvfs(r, s) ? -errno : 0; }

static const struct fuse_operations ops = {
    .getattr = nd_getattr, .readdir = nd_readdir, .mkdir = nd_mkdir, .rmdir = nd_rmdir,
    .unlink = nd_unlink, .rename = nd_rename, .truncate = nd_truncate, .open = nd_open,
    .create = nd_create, .read = nd_read, .write = nd_write, .fsync = nd_fsync,
    .release = nd_release, .statfs = nd_statfs,
};

int main(int argc, char **argv)
{
    if (argc < 3 || !realpath(argv[1], backing)) {
        fprintf(stderr, "usage: %s BACKING_DIR MOUNTPOINT [fuse options]\n", argv[0]);
        return 2;
    }
    argv[1] = argv[0];
    return fuse_main(argc - 1, argv + 1, &ops, NULL);
}
