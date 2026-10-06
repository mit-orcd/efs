#!/usr/bin/env python3
"""Exercise production create result/error mapping with overlapping RPC outcomes."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
ops = (root / 'src/client/ops.c').read_text()
fuse = (root / 'src/client/efs_fuse.c').read_text()
def function(text, name):
    match = re.search(r'^(?:static )?[^\n;]*\b' + name + r'\([^;]+?\)\n\{', text, re.M)
    if not match:
        raise RuntimeError(name)
    return text[match.start():text.index('\n}', match.end()) + 2]
assert 'g_client.last_err' not in fuse
source = r'''
#include "client_internal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct efs_client g_client;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int arrived, lookup_rc, lookups;
static void rendezvous(void) {
    pthread_mutex_lock(&mu);
    ++arrived;
    if (arrived == 16) pthread_cond_broadcast(&cv);
    while (arrived < 16) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}
int efs_client_rpc_create(efs_export_id_t ex, efs_ino_t parent, const char *name,
    uint32_t mode, uid_t uid, gid_t gid, uint32_t flags,
    efs_ino_t *ino, struct efs_inode *out) {
    (void)ex; (void)parent; (void)mode; (void)uid; (void)gid; (void)flags; (void)ino;
    out->ino = 42;
    return !strcmp(name,"busy") ? EFS_ERR_BUSY :
           !strcmp(name,"quota") ? EFS_ERR_QUOTA : EFS_OK;
}
int efs_client_rpc_lookup(efs_export_id_t ex, efs_ino_t parent,
    const char *name, struct efs_inode *out) {
    (void)ex; (void)parent; (void)name; (void)out;
    ++lookups; return lookup_rc;
}
int efs_client_report_admit(uint64_t records) { (void)records; return EFS_OK; }
void efs_client_report_unreserve(void) {}
static void create_stage_inode(efs_ino_t parent, const char *name, uint32_t mode,
    uid_t uid, gid_t gid, const struct efs_inode *out) {
    (void)parent; (void)name; (void)mode; (void)uid; (void)gid;
    assert(out->ino == 42);
}
''' + '\n'.join([function(ops, 'efs_client_create_result'),
                   function(ops, 'efs_client_create_ex'),
                   function(fuse, 'fuse_stat_errno'),
                   function(fuse, 'fuse_create_errno')]) + r'''
static void *worker(void *arg) {
    const char *name = arg;
    efs_ino_t ino = 999;
    int rc = efs_client_create_result(1,name,0,0,0,0,&ino);
    rendezvous(); /* All results exist before any error is consumed. */
    if (!strcmp(name,"busy")) {
        assert(rc == EFS_ERR_BUSY && ino == 0);
        assert(fuse_create_errno(rc,1,name) == -EBUSY);
    } else if (!strcmp(name,"quota")) {
        assert(rc == EFS_ERR_QUOTA && ino == 0);
        assert(fuse_create_errno(rc,1,name) == -ENOSPC);
    } else assert(rc == EFS_OK && ino == 42);
    return NULL;
}
int main(void) {
    pthread_t threads[16];
    const char *names[] = {"busy","quota","ok"};
    g_client.last_err = EFS_OK; /* Unrelated mutable state must not supply errors. */
    for (int i=0;i<16;++i) assert(!pthread_create(&threads[i],NULL,worker,(void *)names[i%3]));
    for (int i=0;i<16;++i) assert(!pthread_join(threads[i],NULL));
    assert(lookups == 0);
    assert(efs_client_create_result(1,"ok",0,0,0,0,NULL) == EFS_ERR_INVAL);
    assert(efs_client_create_ex(1,"busy",0,0,0,0) == 0);
    assert(efs_client_create_ex(1,"ok",0,0,0,0) == 42);
    lookup_rc = EFS_OK;
    assert(fuse_create_errno(EFS_ERR_EXIST,1,"exists") == -EEXIST);
    lookup_rc = EFS_ERR_BUSY;
    assert(fuse_create_errno(EFS_ERR_EXIST,1,"exists") == -EBUSY);
    lookup_rc = EFS_ERR_NOT_FOUND;
    assert(fuse_create_errno(EFS_ERR_EXIST,1,"exists") == -EIO);
    assert(fuse_create_errno(EFS_ERR_IO,1,"failed") == -EIO);
    assert(fuse_create_errno(EFS_ERR_NOT_FOUND,1,"failed") == -ENOENT);
    puts("request-local create errors: concurrent BUSY/quota/success and lookup errors PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-create-errors-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(source)
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-I' + str(root / 'include'), '-I' + str(root / 'src/client')]
    command += shlex.split(os.environ.get('CREATE_TEST_CFLAGS', ''))
    subprocess.run(command + [str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
