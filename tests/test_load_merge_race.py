#!/usr/bin/env python3
"""A completed PUT still owns its body until REPORT; late loaders must fold."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
text = Path(os.environ.get('LOAD_MERGE_SOURCE', root / 'src/client/write.c')).read_text()
def function(name):
    m = re.search(r'^static [^\n;]*\b' + name + r'\([^;]+?\)\n\{', text, re.M)
    return text[m.start():text.index('\n}', m.end()) + 2]
a = text.index('struct dcache_ent {'); entry = text[a:text.index('\n};', a)+3]
a = text.index('struct dcache_init {'); init = text[a:text.index('\n};', a)+3]
store_name = 'dcache_store_owned_locked' if 'dcache_store_owned_locked(' in text else 'dcache_store_owned'
source = r'''
#include "client_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define DCACHE_NR 32
#define DTRACE(...) ((void)0)
#define CS EFS_MIN_CHUNK_SIZE
''' + entry + '\n' + init + r'''
static struct dcache_ent entry;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int replaced, frees, charged, inject;
static void dcache_account_extra(efs_ino_t ino,uint32_t ci) { (void)ino;(void)ci; }
static int observed_unlock(pthread_mutex_t *m) {
    int rc=pthread_mutex_unlock(m);
    if (inject) {
        inject=0;pthread_mutex_lock(m);
        if (!entry.data) entry.data=calloc(1,CS);
        entry.ino=1;entry.len=CS;entry.dirty=entry.pin_held=1;
        memset(entry.data,'A',100);
        entry.roff[entry.nrange]=0;entry.rlen[entry.nrange++]=100;
        pthread_mutex_unlock(m);
    }
    return rc;
}
static uint32_t dcache_slot(efs_ino_t ino,uint32_t ci) { (void)ino;(void)ci;return 0; }
static pthread_mutex_t *dcache_mu(uint32_t s) { (void)s;return &mu; }
static struct dcache_ent *dcache_find(uint32_t s,efs_ino_t ino,uint32_t ci) {
    (void)s;return entry.data && entry.ino==ino && entry.ci==ci ? &entry : NULL;
}
static void dcache_set_dirty(struct dcache_ent *e,uint32_t s) { (void)s;e->dirty=1; }
static void dcache_pin_add(struct dcache_ent *e) { e->pin_held=1; }
static void dcache_note_dirty_bytes(int64_t bytes) { assert(bytes==CS);++charged; }
void efs_buf_free(void *p,uint32_t n) { assert(n==CS);++frees;free(p); }
void efs_rdcache_invalidate(efs_ino_t ino,uint32_t ci) { (void)ino;(void)ci; }
static int dcache_store_owned(efs_ino_t ino,uint32_t ci,uint8_t *chunk,
                               uint32_t cs,const struct dcache_init *in) {
    (void)ino;(void)ci;(void)cs; STORE_LOCK;
    ++replaced;free(entry.data);entry.data=chunk;entry.ino=1;entry.len=CS;
    entry.dirty=entry.pin_held=1;entry.nrange=1;entry.roff[0]=in->off;entry.rlen[0]=in->len;
    STORE_UNLOCK; return 0;
}
''' .replace('dcache_store_owned(',store_name+'(').replace('STORE_LOCK;', 'assert(pthread_mutex_trylock(&mu)!=0);' if store_name.endswith('_locked') else 'pthread_mutex_lock(&mu);').replace('STORE_UNLOCK;', '' if store_name.endswith('_locked') else 'pthread_mutex_unlock(&mu);') + '\n#define pthread_mutex_unlock observed_unlock\n' + '\n'.join(function(n) for n in ['dcache_keep_on_drop','dcache_add_range','dcache_merge_owned']) + r'''
#undef pthread_mutex_unlock
int main(void) {
    for (unsigned mode=0;mode<3;++mode) {
        memset(&entry,0,sizeof(entry));replaced=frees=charged=0;
        entry.ino=1;entry.data=calloc(1,CS);entry.len=CS;entry.pin_held=1;
        memset(entry.data,'A',100);entry.nrange=1;entry.rlen[0]=100;
        entry.object_gen=100;entry.committed_object=99;entry.object_seq=2;entry.committed_seq=1;
        if (mode==1) {entry.object_gen=entry.committed_object=100;entry.pin_held=1;}
        if (mode==2) entry.dirty=1;
        uint8_t src[100];memset(src,'B',sizeof(src));
        uint8_t *loaded=calloc(1,CS);memcpy(loaded+200,src,100);
        struct dcache_init in={0,0,0,200,100};
        assert(dcache_merge_owned(1,0,200,src,100,loaded,CS,&in)==1);
        assert(!replaced && frees==1 && entry.dirty && entry.pin_held);
        assert(charged==(mode!=2) && entry.nrange==2 && entry.object_gen==100);
        for (unsigned i=0;i<100;++i) assert(entry.data[i]=='A' && entry.data[200+i]=='B');
        assert(!pthread_mutex_trylock(&mu));pthread_mutex_unlock(&mu);free(entry.data);
    }
    memset(&entry,0,sizeof(entry));inject=1;
    uint8_t src[100];memset(src,'B',100);uint8_t *loaded=calloc(1,CS);memcpy(loaded+200,src,100);
    struct dcache_init in={0,0,0,200,100};
    assert(dcache_merge_owned(1,0,200,src,100,loaded,CS,&in)==0);
    for (unsigned i=0;i<100;++i) assert(entry.data[i]=='A' && entry.data[200+i]=='B');
    assert(entry.nrange==2);free(entry.data);
    puts("load merge: pending PUT, retained pin and dirty ownership survive late loaders PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-load-merge-') as directory:
    path = Path(directory) / 'test.c'; path.write_text(source)
    command = shlex.split(os.environ.get('CC','cc')) + ['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client')]
    command += shlex.split(os.environ.get('LOAD_MERGE_CFLAGS',''))
    subprocess.run(command+[str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
