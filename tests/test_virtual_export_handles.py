#!/usr/bin/env python3
"""Actual bounded virtual identities survive FORGET until safely recycled.

NFS re-export can decode a fresh file handle after entry invalidation sends
FORGET. The callback recovery path is covered by test_lookup_errno.py.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/client/efs_fuse.c').read_text()
def function(name):
    match = re.search(r'^static [^\n;]*\b' + name + r'\([^;]+?\)\n\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.end()) + 2]
state = source[source.index('#define EFS_VQ_N'):source.index('static uint64_t virtual_version')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
typedef uint64_t efs_ino_t;
typedef uint64_t fuse_ino_t;
#define EFS_MAX_NAME 256
#define EFS_VIRT_QUERY (1ULL<<60)
#define EFS_VIRT_PARENT_MASK ((1ULL<<60)-1)
''' + state + '\n'.join(function(name) for name in (
    'virtual_version', 'vq_intern', 'vq_lookup', 'vq_nlookup_add',
    'vq_nlookup_sub', 'vq_open_size', 'vq_pinned_size', 'vq_version_matches')) + r'''
int main(void) {
    const char *bytes="original"; size_t length=0;efs_ino_t dir=0;char term[256];
    fuse_ino_t old=vq_intern(42,"v1.stats.nonce",virtual_version(bytes,8),8);
    assert(old);vq_nlookup_add(old,1);vq_nlookup_sub(old,1);
    assert(!vq_lookup(old,&dir,term,sizeof(term)) && dir==42 && !strcmp(term,"v1.stats.nonce"));
    assert(vq_intern(42,"v1.stats.nonce",virtual_version(bytes,8),8)==old);
    assert(vq_pinned_size(old,&length,0) && length==8);
    assert(!vq_pinned_size(old,&length,1));
    assert(vq_version_matches(old,bytes,8) && !vq_version_matches(old,"changed!",8));
    vq_open_size(old,8,1);vq_nlookup_add(old,2);vq_nlookup_sub(old,9);
    assert(vq_pinned_size(old,&length,1) && length==8);
    for(unsigned i=1;i<EFS_VQ_N;i++) {
        char name[64];snprintf(name,sizeof(name),"v1.stats.%u",i);
        fuse_ino_t id=vq_intern(42,name,1,8);assert(id && id!=old);
        vq_nlookup_add(id,1);
    }
    assert(!vq_intern(42,"overflow",1,8)); /* all 64 lookup/open pins held */
    vq_open_size(old,8,-1);
    assert(!vq_lookup(old,NULL,NULL,0)); /* CLOSE alone preserves its identity */
    fuse_ino_t replacement=vq_intern(42,"replacement",2,9);
    assert(replacement && replacement!=old);
    assert(vq_lookup(old,NULL,NULL,0)); /* recycled identity never aliases */
    assert(!vq_lookup(replacement,&dir,term,sizeof(term)) && !strcmp(term,"replacement"));
    assert(vq_pinned_size(replacement,&length,0) && length==9);
    assert(!vq_version_matches(replacement,bytes,8));
    puts("virtual export handles: FORGET recovery, immutable version/size, open/lookup pins, bounded 64-slot recycling PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-virtual-export-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(code)
    binary = path.with_suffix('')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(path), '-lpthread', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
