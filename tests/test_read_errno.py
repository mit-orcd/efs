#!/usr/bin/env python3
"""Exercise the production FUSE read handler with typed client failures."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('EFS_FUSE_SOURCE', root / 'src/client/efs_fuse.c')).read_text()

def function(name):
    match = re.search(r'^static int ' + name + r'\([^;]+?\)\n\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.end()) + 2]

code = r'''
#include <efs/metadata.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
typedef uint64_t fuse_ino_t;
struct fuse_file_info { uint64_t fh; };
struct efs_file_handle { char *snapshot; size_t snapshot_len; };
struct stats_ent { char text[1]; int len; };
static int read_rc, calls, logs, unaligned;
static uint64_t seen_ino, seen_offset;
static size_t seen_size;
static int virt_kind(fuse_ino_t ino) { (void)ino; return 0; }
static struct efs_file_handle *efs_file_handle(struct fuse_file_info *fi)
{ (void)fi; return NULL; }
static int feature_enabled(int feature) { (void)feature; return 0; }
static int stats_snapshot(uint64_t ino, struct stats_ent *e)
{ (void)ino; (void)e; return -1; }
static uint64_t virt_parent(uint64_t ino) { return ino; }
static int commands_enabled(void) { return 0; }
static int vq_lookup(uint64_t ino, uint64_t *dir, char *term, size_t size)
{ (void)ino; (void)dir; (void)term; (void)size; return -1; }
#define EFS_FIND_NAME ".find"
static int find_query_read(uint64_t ino, const char *path, const char *term,
                           char *buf, size_t size, off_t off)
{ (void)ino; (void)path; (void)term; (void)buf; (void)size; (void)off; return -EIO; }
static uint64_t efs_file_ino(struct fuse_file_info *fi, uint64_t ino)
{ return fi ? fi->fh : ino; }
static int fuse_odirect_unaligned(struct fuse_file_info *fi, off_t off, size_t size)
{ (void)fi; (void)off; (void)size; return unaligned; }
static int efs_client_read(uint64_t ino, uint64_t off, size_t size,
                           void *buf, size_t *got)
{
    calls++; seen_ino=ino; seen_offset=off; seen_size=size;
    if (!read_rc) { memcpy(buf,"valid",5); *got=5; }
    return read_rc;
}
static void efs_fuse_log_err(const char *where, int rc, uint64_t ino,
                             uint64_t off, size_t size, const char *path)
{ (void)where; (void)rc; (void)ino; (void)off; (void)size; (void)path; logs++; }
''' + function('efs_rc_to_errno') + function('efs_fuse_read_ino') + r'''
int main(void)
{
    assert(efs_rc_to_errno(EFS_ERR_NOMEM)==-ENOMEM);
    int cases[][2]={{EFS_ERR_NOMEM,-ENOMEM},{EFS_ERR_IO,-EIO},
        {EFS_ERR_NET,-EIO},{EFS_ERR_NO_QUORUM,-EIO},
        {EFS_ERR_BUSY,-EBUSY},{EFS_ERR_AGAIN,-EAGAIN},
        {EFS_ERR_ACCES,-EACCES},{EFS_ERR_INVAL,-EINVAL},
        {EFS_ERR_NOT_FOUND,-ENOENT},{EFS_ERR_ILSEQ,-EILSEQ}};
    char buf[32]={0}; struct fuse_file_info fi={.fh=99};
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        read_rc=cases[i][0];
        assert(efs_fuse_read_ino(42,buf,sizeof(buf),7,&fi)==cases[i][1]);
        assert(seen_ino==99 && seen_offset==7 && seen_size==sizeof(buf));
        assert(calls==(int)i+1 && logs==calls && !buf[0]);
    }
    read_rc=0;
    assert(efs_fuse_read_ino(42,buf,sizeof(buf),7,&fi)==5);
    assert(!memcmp(buf,"valid",5) && logs==calls-1);
    assert(efs_fuse_read_ino(42,buf,sizeof(buf),7,NULL)==5 && seen_ino==42);
    int before=calls; unaligned=1;
    assert(efs_fuse_read_ino(42,buf,sizeof(buf),7,&fi)==-EINVAL && calls==before);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-errno-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(code)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'include'), str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True, timeout=10)
print('FUSE READ: allocation ENOMEM, typed failures, successful bytes and alignment preserved PASS')
