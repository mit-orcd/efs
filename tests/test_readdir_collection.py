#!/usr/bin/env python3
"""Execute real FUSE listing collection: shard duplicates, cost and allocation failures."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('EFS_FUSE_SOURCE', root / 'src/client/efs_fuse.c')).read_text()
structs = source[source.index('struct readdir_ent {'):source.index('/* Per-opendir listing.')]
start = source.find('static size_t readdir_name_hash(')
if start < 0:
    start = source.index('static int readdir_collect_page(')
end = source.index('\nstatic int efs_fuse_readdir_ino(', start)
functions = source[start:end]
code = r'''
#include <efs/metadata.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static size_t comparisons;
static int fail_calloc, fail_realloc;
static int observed_strcmp(const char *a,const char *b){comparisons++;return strcmp(a,b);}
static void *observed_calloc(size_t n,size_t z){if(fail_calloc){fail_calloc=0;return NULL;}return calloc(n,z);}
static void *observed_realloc(void *p,size_t z){if(fail_realloc){fail_realloc=0;return NULL;}return realloc(p,z);}
#define strcmp observed_strcmp
#define calloc observed_calloc
#define realloc observed_realloc
''' + structs + functions + r'''
#undef strcmp
#undef calloc
#undef realloc
static void release(struct readdir_collect_arg *c){
 free(c->ents);
#ifdef HAS_NAME_INDEX
 free(c->name_index);
#endif
 memset(c,0,sizeof(*c));
}
static void row(struct efs_inode *r,const char *name,uint64_t ino){
 memset(r,0,sizeof(*r));snprintf(r->name,sizeof(r->name),"%s",name);r->ino=ino;r->mode=S_IFREG|0600;
}
int main(void){
 struct readdir_collect_arg c={0};struct efs_inode page[64];
 for(unsigned base=0;base<4096;base+=64){
  for(unsigned j=0;j<64;j++){char name[32];snprintf(name,sizeof(name),"file_%06u",base+j);row(&page[j],name,base+j+1);}
  assert(!readdir_collect_page(&c,page,64));
 }
 assert(c.count==4096);
 for(unsigned i=0;i<4096;i++){char name[32];snprintf(name,sizeof(name),"file_%06u",i);assert(!strcmp(c.ents[i].name,name));assert(c.ents[i].st.st_ino==i+1);}
 for(unsigned base=0;base<4096;base+=64){
  for(unsigned j=0;j<64;j++){char name[32];snprintf(name,sizeof(name),"file_%06u",base+j);row(&page[j],name,99999);page[j].mode=S_IFDIR|0700;}
  assert(!readdir_collect_page(&c,page,64));
 }
 assert(c.count==4096);
 for(unsigned i=0;i<4096;i++){assert(c.ents[i].st.st_ino==i+1);assert(c.ents[i].st.st_mode==(S_IFREG|0600));}
 printf("4096 unique names plus duplicate shard pages: %zu name comparisons\n",comparisons);fflush(stdout);
 assert(comparisons<65536);
 row(&page[0],".fuse_hidden123",1);row(&page[1],".parked-test",1);row(&page[2],"",1);row(&page[3],"zero-inode",0);
 assert(!readdir_collect_page(&c,page,4));assert(c.count==4096);assert(!readdir_collect_page(&c,NULL,0));
 release(&c);
#ifdef HAS_NAME_INDEX
 /* Deliberate bucket collisions still preserve distinct names and first rows. */
 unsigned found=0;
 for(unsigned i=0;found<12;i++){char name[32];snprintf(name,sizeof(name),"collision_%u",i);if((readdir_name_hash(name)&31)==0)row(&page[found++],name,i+1);}
 assert(!readdir_collect_page(&c,page,12));assert(c.count==12);
 for(unsigned j=0;j<12;j++)assert(!strcmp(c.ents[j].name,page[j].name));
 assert(!readdir_collect_page(&c,page,12));assert(c.count==12);release(&c);
 row(&page[0],"first",1);
 fail_calloc=1;assert(readdir_collect_page(&c,page,1)==-ENOMEM);assert(c.count==0);assert(!c.name_index);assert(!c.ents);
 fail_realloc=1;assert(readdir_collect_page(&c,page,1)==-ENOMEM);assert(c.count==0);assert(c.name_index);assert(!c.ents);
 assert(!readdir_collect_page(&c,page,1));assert(c.count==1);
 size_t *saved=c.name_index;
 for(unsigned j=0;j<64;j++){char name[32];snprintf(name,sizeof(name),"next_%u",j);row(&page[j],name,j+2);}
 fail_calloc=1;assert(readdir_collect_page(&c,page,64)==-ENOMEM);assert(c.count==1);assert(c.name_index==saved);assert(!strcmp(c.ents[0].name,"first"));
 fail_realloc=1;assert(readdir_collect_page(&c,page,64)==-ENOMEM);assert(c.count==1);assert(!strcmp(c.ents[0].name,"first"));
 assert(!readdir_collect_page(&c,page,64));assert(c.count==65);
 size_t count=c.count;c.count=SIZE_MAX;assert(readdir_collect_page(&c,page,1)==-ENOMEM);c.count=count;
 assert(readdir_name_reserve(&c,SIZE_MAX)==-ENOMEM);release(&c);
#else
 (void)observed_calloc;
#endif
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-readdir-collection-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(code)
    flags = shlex.split(os.environ.get('READDIR_TEST_CFLAGS', ''))
    if 'name_index_cap' in structs:
        flags.append('-DHAS_NAME_INDEX')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-O2', '-Wall', '-Wextra', '-Werror'] + flags + ['-I' + str(root / 'include'), str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True, timeout=30)
print('Readdir collection: first duplicate retained, bounded comparisons, collisions, hidden names, allocation failures and overflow PASS')
