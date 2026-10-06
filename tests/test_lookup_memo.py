#!/usr/bin/env python3
"""Exercise production memo and FUSE callbacks with deterministic RPC stubs.
This is a local regression gate, not the Linux posix/Spark integration gate.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
fuse = (root / "src/client/efs_fuse.c").read_text()

def function(name):
    original = name
    if re.search(r"^static void " + name + r"_run\(", fuse, re.M):
        wrapper = fuse[fuse.index("static void " + name + "("): ]
        assert "efs_stop_mutation_enter()" in wrapper[:wrapper.index("\n}")], name
        name += "_run"
    match = re.search(r"^static [^\n;]*\b" + name + r"\([^;]+?\)\n\{", fuse, re.M)
    if not match:
        raise RuntimeError(name)
    return fuse[match.start():fuse.index("\n}", match.end()) + 2].replace(name + "(", original + "(", 1)

memo = fuse[fuse.index("#define LOOKUP_MEMO_N"):fuse.index("static void getattr_note_row")]
callbacks = ["ll_write", "ll_write_buf", "ll_setattr", "ll_link", "ll_unlink",
             "ll_rename", "ll_open", "ll_fallocate"]
source = r'''
#include "efs/metadata.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef uint64_t fuse_ino_t;
typedef void *fuse_req_t;
struct fuse_file_info { uint64_t fh; int flags; };
struct fuse_bufvec { int unused; };
struct fuse_entry_param { int unused; };
#define FUSE_SET_ATTR_SIZE 1
#define FALLOC_FL_KEEP_SIZE 1
static fuse_req_t t_req;
''' + memo + r'''
static struct efs_inode current = {.ino=12, .mode=S_IFREG|0644, .nlink=1};
static int failure, stat_failure, vk;
static void check_active(void) { assert(g_lookup_memo_active > 0); }
static int efs_fuse_write(const char *p,const char *b,size_t n,off_t o,struct fuse_file_info *fi) {
    (void)p;(void)b;(void)o;(void)fi;check_active();
    if(failure) return -EIO;
    current.size+=n; return (int)n;
}
static int efs_fuse_write_buf(const char *p,struct fuse_bufvec *b,off_t o,struct fuse_file_info *fi) {
    (void)b;return efs_fuse_write(p,"",4,o,fi);
}
static int efs_fuse_setattr_ino(fuse_ino_t ino,struct stat *a,int flags,struct fuse_file_info *fi) {
    (void)ino;(void)flags;(void)fi;check_active();
    if(failure) return -EIO;
    current.size=a->st_size;current.mode=a->st_mode;current.mtime=42;return 0;
}
static int efs_fuse_getattr_ino(fuse_ino_t ino,struct stat *a,struct fuse_file_info *fi) {
    struct efs_inode row;
    (void)fi;
    /* SETATTR's reply GETATTR must bypass the stale memo while active. */
    assert(!lookup_memo_take(ino,&row));
    a->st_size=current.size;a->st_mode=current.mode;return 0;
}
static int efs_fuse_link_at(fuse_ino_t ino,fuse_ino_t parent,const char *name) {
    (void)ino;(void)parent;(void)name;check_active();
    if(failure) return -EIO;
    ++current.nlink;return 0;
}
static int lookup_fill_committed(fuse_ino_t ino,struct fuse_entry_param *e,
                                 const char *op,fuse_ino_t parent,const char *name) {
    (void)ino;(void)e;(void)op;(void)parent;(void)name;
    check_active();lookup_memo_put(&current);return 0;
}
static int efs_fuse_unlink_at(fuse_ino_t parent,const char *name) {
    (void)parent;(void)name;check_active();if(failure)return -EIO;
    --current.nlink;return 0;
}
static int efs_fuse_rename_at(fuse_ino_t parent,const char *name,fuse_ino_t dest,
                            const char *newname,unsigned flags,efs_ino_t *out) {
    (void)parent;(void)name;(void)dest;(void)newname;(void)flags;
    check_active();*out=current.ino;return failure ? -EIO : 0;
}
static int efs_fuse_open_ino(fuse_ino_t ino,struct fuse_file_info *fi) {
    (void)ino;if(fi->flags&O_TRUNC) {check_active();if(!failure)current.size=0;}
    return failure ? -EIO : 0;
}
static int virt_kind(fuse_ino_t ino) { (void)ino;return vk; }
static int efs_append_flush_report(struct fuse_file_info *fi,efs_ino_t ino) {
    (void)fi;(void)ino;check_active();return failure ? EFS_ERR_IO : EFS_OK;
}
static int efs_client_stat_ino(efs_ino_t ino,struct efs_inode *out) {
    (void)ino;*out=current;return stat_failure ? EFS_ERR_IO : EFS_OK;
}
static int efs_fuse_truncate_ino(fuse_ino_t ino,off_t size,struct fuse_file_info *fi) {
    (void)ino;(void)fi;check_active();current.size=size;return 0;
}
static int efs_rc_to_errno(int rc) { return rc==EFS_OK ? 0 : -EIO; }
static void ll_inval_inode(fuse_ino_t ino,off_t off,off_t len) { (void)ino;(void)off;(void)len; }
static void fuse_reply_err(fuse_req_t r,int e) { (void)r;(void)e; }
static void fuse_reply_write(fuse_req_t r,size_t n) { (void)r;(void)n; }
static void fuse_reply_attr(fuse_req_t r,struct stat *s,double timeout) { (void)r;(void)s;(void)timeout; }
static void fuse_reply_entry(fuse_req_t r,struct fuse_entry_param *e) { (void)r;(void)e; }
static void fuse_reply_open(fuse_req_t r,struct fuse_file_info *fi) { (void)r;(void)fi; }
''' + "\n".join(function(name) for name in callbacks) + r'''
static void seed(void) {
    t_lookup_memo_serial=lookup_memo_start();
    lookup_memo_put(&current);
}
static void missed(void) {
    struct efs_inode out;
    assert(!g_lookup_memo_active);
    assert(!lookup_memo_take(current.ino,&out));
}
static pthread_mutex_t gate_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv=PTHREAD_COND_INITIALIZER;
static int captured, resume;
static void *old_lookup(void *unused) {
    (void)unused;
    t_lookup_memo_serial=lookup_memo_start();
    pthread_mutex_lock(&gate_mu);
    captured=1;pthread_cond_broadcast(&gate_cv);
    while(!resume)pthread_cond_wait(&gate_cv,&gate_mu);
    pthread_mutex_unlock(&gate_mu);
    lookup_memo_put(&current); /* old RPC completes after mutation */
    return NULL;
}
int main(void) {
    struct efs_inode out;
    struct fuse_file_info fi={.fh=12};
    struct fuse_bufvec bv={0};
    struct stat attr={.st_size=17,.st_mode=S_IFREG|0600};
    seed();assert(lookup_memo_take(12,&out));assert(!lookup_memo_take(12,&out));
    seed();g_lookup_memo[(g_lookup_memo_i-1)%LOOKUP_MEMO_N].us-=LOOKUP_MEMO_US;
    assert(!lookup_memo_take(12,&out));
    for(int fail=0;fail<2;++fail) {
        failure=fail;
        seed();ll_write(NULL,12,"abc",3,0,&fi);missed();
        seed();ll_write_buf(NULL,12,&bv,0,&fi);missed();
        seed();ll_setattr(NULL,12,&attr,FUSE_SET_ATTR_SIZE,&fi);missed();
        seed();ll_link(NULL,12,1,"link");missed();
        seed();ll_unlink(NULL,1,"link");missed();
        seed();ll_rename(NULL,1,"a",2,"b",0);missed();
        fi.flags=O_TRUNC;seed();ll_open(NULL,12,&fi);missed();
        seed();ll_fallocate(NULL,12,0,0,100,&fi);missed();
    }
    failure=0;fi.flags=0;
    seed();ll_open(NULL,12,&fi);assert(lookup_memo_take(12,&out)); /* read-only hit retained */
    for(int branch=0;branch<7;++branch) {
        vk=branch==0;stat_failure=branch==3;
        current.mode=branch==4 ? S_IFDIR|0755 : S_IFREG|0644;
        current.size=branch==5 ? 200 : 0;
        int mode=branch==2 ? 8 : branch==6 ? FALLOC_FL_KEEP_SIZE : 0;
        seed();ll_fallocate(NULL,12,mode,branch==1 ? -1 : 0,100,&fi);missed();
    }
    lookup_memo_mutation_begin();lookup_memo_mutation_begin();
    seed();lookup_memo_mutation_end();assert(!lookup_memo_take(12,&out));
    seed();lookup_memo_mutation_end();lookup_memo_put(&current);missed();
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,old_lookup,NULL));
    pthread_mutex_lock(&gate_mu);
    while(!captured)pthread_cond_wait(&gate_cv,&gate_mu);
    pthread_mutex_unlock(&gate_mu);
    lookup_memo_mutation_begin();lookup_memo_mutation_end();
    pthread_mutex_lock(&gate_mu);resume=1;pthread_cond_broadcast(&gate_cv);pthread_mutex_unlock(&gate_mu);
    assert(!pthread_join(thread,NULL));missed();
    seed();assert(lookup_memo_take(12,&out)); /* fresh LOOKUP recovers fast path */
    puts("lookup memo: mutation callbacks, errors, overlap, stale RPC, TTL and read-only hits PASS");
    return 0;
}
'''

# Remaining entrypoints and asynchronous writeback must carry the guard too.
for name in ["ll_mkdir", "ll_rmdir", "ll_symlink", "ll_mknod", "ll_create",
             "ll_setxattr", "ll_removexattr", "ll_flush", "ll_fsync", "ll_release"]:
    body = function(name)
    assert "lookup_memo_mutation_begin();" in body, name
    assert "lookup_memo_mutation_end();" in body, name
assert "lookup_memo_mutation_begin();" in function("efs_wb_thread")
assert "lookup_memo_mutation_end();" in function("efs_wb_thread")
assert "t_lookup_memo_serial = lookup_memo_start();" in function("ll_lookup")
assert "t_lookup_memo_serial = 0;" in function("ll_lookup")

with tempfile.TemporaryDirectory(prefix="efs-memo-") as directory:
    path = Path(directory) / "test.c"
    path.write_text(source)
    command = shlex.split(os.environ.get("CC", "cc"))
    command += ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-variable", "-pthread", "-I" + str(root / "include")]
    command += shlex.split(os.environ.get("MEMO_TEST_CFLAGS", ""))
    subprocess.run(command + [str(path), "-o", str(path.with_suffix(""))], check=True)
    subprocess.run([str(path.with_suffix(""))], check=True)
