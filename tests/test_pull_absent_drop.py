#!/usr/bin/env python3
"""A successful GETCHUNKS is authority for the chunks it covered and did not
return: their local entries are dropped (a peer truncate deleted the rows),
unless a local publish replaced the entry since the snapshot (D25 g4.3)."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/ops.c').read_text()
def fn(name):
 m=re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{',s,re.M);return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include "client_internal.h"
#include <assert.h>
#include <string.h>
struct efs_client g_client;
static uint64_t key[256];static int dropped[256],invalid[256];
void efs_client_lock_dir(efs_ino_t i){(void)i;}
void efs_client_unlock_dir(efs_ino_t i){(void)i;}
int efs_export_get_chunk(struct efs_export *e,efs_ino_t ino,uint32_t ci,struct efs_chunk_entry *ce){(void)e;assert(ino==9);if(!key[ci])return EFS_ERR_NOT_FOUND;memset(ce,0,sizeof(*ce));ce->generation=key[ci];return 0;}
int efs_export_drop_chunk(struct efs_export *e,efs_ino_t ino,uint32_t ci){(void)e;assert(ino==9);dropped[ci]++;key[ci]=0;return 1;}
void efs_rdcache_invalidate(efs_ino_t ino,uint32_t ci){assert(ino==9);invalid[ci]++;}
static uint64_t owned_gen[256];
int efs_client_putid_owns(efs_ino_t ino,uint32_t ci,uint64_t gen){assert(ino==9);return gen&&owned_gen[ci]==gen;}
'''+fn('map_snapshot')+fn('map_drop_absent')+r'''
static struct efs_chunk_rec rec(uint32_t ci){struct efs_chunk_rec r;memset(&r,0,sizeof(r));r.ino=9;r.chunk_index=ci;return r;}
int main(void){
 uint64_t before[64];struct efs_chunk_rec r[64];
 for(int c=0;c<64;c++)key[c]=100+c;
 map_snapshot(9,0,64,before);
 /* Reply returned 3 and 10 of 64 asked: it scanned the whole group. */
 r[0]=rec(3);r[1]=rec(10);
 key[5]=999; /* a local publish replaced chunk 5 during the RPC */
 owned_gen[7]=key[7]; /* chunk 7 is this client's PUT awaiting its REPORT */
 map_drop_absent(9,0,64,r,2,64,before);
 assert(!dropped[3]&&!dropped[10]&&!invalid[3]);
 assert(!dropped[5]&&key[5]==999);
 assert(dropped[0]&&dropped[4]&&dropped[63]&&invalid[63]);
 assert(!dropped[7]&&key[7]==107); /* own unreported PUT kept for its replay */
 /* A full reply covers only up to its last record. */
 memset(dropped,0,sizeof(dropped));for(int c=64;c<128;c++)key[c]=100+c;
 map_snapshot(9,64,128,before);
 for(uint32_t i=0;i<64;i++)r[i]=rec(64+i);
 map_drop_absent(9,64,128,r,64,64,before);
 for(int c=64;c<128;c++)assert(!dropped[c]);
 /* Empty reply: the whole group is absent; never-seen entries untouched. */
 key[130]=0;key[131]=7;map_snapshot(9,128,192,before);key[130]=5;
 map_drop_absent(9,128,192,r,0,64,before);
 assert(dropped[131]&&!dropped[130]&&key[130]==5);
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-pull-absent-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('GETCHUNKS absence: covered absent entries dropped; replaced, own unreported and unseen entries kept; full-reply bound PASS')
