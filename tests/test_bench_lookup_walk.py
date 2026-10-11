#!/usr/bin/env python3
"""Exercise the actual benchmark walker with deterministic owner-RPC stubs."""
from pathlib import Path
import argparse
import os
import re
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / 'src/client/ops.c')
args = parser.parse_args()
text = args.source.read_text()
def function(name):
    match = re.search(r'^(?:static )?[^\n;]*\b' + name + r'\([^;]+?\)\n\{', text, re.M)
    if not match:
        raise RuntimeError(name)
    return text[match.start():text.index('\n}', match.end()) + 2]
source = r'''
#include "client_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct efs_client g_client;
static unsigned lookups, batches, adopts, overlays, locks;
static int batch_mode, lookup_error;
void efs_client_ensure_dir_locks(void) {}
void efs_client_lock_dir(efs_ino_t ino) { assert(ino == EFS_ROOT_INO); locks++; }
void efs_client_unlock_dir(efs_ino_t ino) { assert(ino == EFS_ROOT_INO && locks); locks--; }
int efs_export_get_inode(struct efs_export *e, efs_ino_t ino, struct efs_inode *out) {
    (void)e; memset(out,0,sizeof(*out));out->ino=ino;out->mode=S_IFDIR;return EFS_OK;
}
int efs_client_rpc_lookup(efs_export_id_t id, efs_ino_t parent, const char *name, struct efs_inode *out) {
    assert(id == 7 && name[0]);lookups++;
    if (lookup_error) return lookup_error;
    memset(out,0,sizeof(*out));out->ino=parent+1;out->mode=S_IFDIR;out->uid=999;
    return EFS_OK;
}
int efs_client_rpc_lookup_path(efs_export_id_t id, efs_ino_t start, const char *path,
                             uint32_t flags, struct efs_msg_inode_lookup_path_reply *out) {
    assert(id == 7 && flags == EFS_LOOKUP_PATH_F_ANCESTORS);batches++;
    if (batch_mode == 1) return EFS_ERR_IO;
    unsigned count=0;for(const char *p=path;*p;p++)count+=(*p=='/');
    assert(count && count<=EFS_LOOKUP_PATH_MAX_DEPTH);
    memset(out,0,sizeof(*out));out->inode.ino=start+count;out->inode.mode=S_IFDIR;
    out->ancestor_count=batch_mode==2?count:count-1;
    return EFS_OK;
}
static void adopt_rpc_inode(const struct efs_inode *row) { assert(row->ino);adopts++; }
static void overlay_local_size(struct efs_inode *row, int more) {
    overlays++;if(!more)row->size=777;
}
#define EFS_WALK_MAX_COMPS 2048
#define EFS_WALK_BATCH_MIN 8
'''
if re.search(r'^static int lookup_access_ok\(', text, re.M):
    source += function('lookup_access_ok') + '\n'
for name in ('walk_batch_ancestors', 'lookup_walk', 'efs_client_lookup'):
    source += function(name) + '\n'
source += r'''
static void walk(unsigned depth,int mode) {
    char path[4096]="";for(unsigned i=0;i<depth;i++)strcat(path,"/x");
    batch_mode=mode;lookup_error=0;lookups=batches=adopts=overlays=0;
    struct efs_inode row;
    assert(efs_client_lookup(path,&row)==EFS_OK);
    assert(row.ino==EFS_ROOT_INO+depth && row.size==777);
    assert(adopts==lookups && overlays==lookups);
    unsigned expected=depth>8?(depth-2)/EFS_LOOKUP_PATH_MAX_DEPTH+1:0;
    assert(batches==(mode&&depth>8?1:expected));
    assert(lookups==(depth>8&&!mode?1:depth));
}
int main(void) {
    g_client.export_id=7;
    struct efs_inode row;
    assert(efs_client_lookup(NULL,&row)==EFS_ERR_INVAL);
    assert(efs_client_lookup("relative",&row)==EFS_ERR_INVAL);
    assert(efs_client_lookup("//",&row)==EFS_ERR_INVAL);
    char too_long[4098];memset(too_long,'x',sizeof(too_long));too_long[0]='/';too_long[4097]=0;
    assert(efs_client_lookup(too_long,&row)==EFS_ERR_INVAL);
    assert(efs_client_lookup("/",&row)==EFS_OK && row.ino==EFS_ROOT_INO && !locks);
    assert(efs_client_lookup("//x///x/",&row)==EFS_OK && row.ino==EFS_ROOT_INO+2);
    for(int mode=0;mode<=2;mode++) {
        walk(1,mode);walk(8,mode);walk(9,mode);walk(66,mode);walk(140,mode);
    }
    int errors[]={EFS_ERR_NOT_FOUND,EFS_ERR_INVAL,EFS_ERR_ACCES,EFS_ERR_IO,EFS_ERR_NET,EFS_ERR_BUSY};
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        lookup_error=errors[i];batch_mode=0;
        assert(efs_client_lookup("/missing/leaf",&row)==errors[i]);
    }
    puts("benchmark walker: bounds/root/slashes, shallow/deep batches, malformed/error fallback, owner errors and leaf overlay PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-bench-walk-') as tmp:
    c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(source)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+
                   ['-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror',
                    '-I'+str(root/'include'),'-I'+str(root/'src/client'),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
