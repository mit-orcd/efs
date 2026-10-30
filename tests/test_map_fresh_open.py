#!/usr/bin/env python3
"""Read-mapping TTL: fresh within the TTL, stale after it, and close-to-open:
an open() makes every mapping pulled before it (or in the same ms) stale."""
from pathlib import Path
import subprocess,tempfile,sys
root=Path(__file__).resolve().parents[1];s=Path(sys.argv[1] if len(sys.argv)>1 else root/'src/client/ops.c').read_text()
a=s.index('#define MAP_FRESH_SLOTS');b=s.index('static int pull_chunks_range')
block=s[a:b].replace('static uint64_t map_fresh_now_us(void)\n{','static uint64_t map_fresh_now_us_real(void)\n{')
start=s.index('static void invalidate_file_layout(');invalidate=s[start:s.index('\n}',start)+2]
code=r'''
#include "client_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <time.h>
static uint64_t now_us=1000000;
static uint64_t map_fresh_now_us(void);
'''+block+r'''
static uint32_t data_chunk_size(void){return 131072;}
static unsigned invalidates,drops;
void efs_rdcache_invalidate(efs_ino_t ino,uint32_t ci){(void)ino;(void)ci;invalidates++;}
void efs_dcache_drop_if_clean(efs_ino_t ino,uint32_t ci){(void)ino;(void)ci;drops++;}
'''+invalidate+r'''
static uint64_t map_fresh_now_us(void) {return now_us;}
int main(void) {
 (void)map_fresh_now_us_real;
 map_fresh_mark(5,0,4,now_us);
 now_us+=150000;assert(efs_client_map_fresh(5,0,4));
 assert(!efs_client_map_fresh(5,0,5));      /* ci 4 never pulled */
 assert(!efs_client_map_fresh(6,0,1));      /* other inode */
 now_us+=60000;assert(!efs_client_map_fresh(5,0,1)); /* past 200 ms */
 map_fresh_mark(5,0,4,now_us);now_us+=1;assert(efs_client_map_fresh(5,0,4));
 efs_client_map_opened(5);                  /* close-to-open */
 assert(!efs_client_map_fresh(5,0,1));
 map_fresh_mark(5,0,4,now_us);              /* same tick as the open */
 assert(!efs_client_map_fresh(5,0,1));
 now_us+=1;map_fresh_mark(5,0,4,now_us);    /* one microsecond later */
 assert(efs_client_map_fresh(5,0,4));
 /* A fresh inode size/epoch cannot reuse a map stamped before its
  * invalidation, including an in-flight old GETCHUNKS arriving late. */
 uint64_t before=now_us;map_fresh_mark(55,0,4,before);now_us++;
 assert(efs_client_map_fresh(55,0,4));
 struct efs_inode inode={.ino=55,.size=0};invalidate_file_layout(&inode,1);
 assert(!efs_client_map_fresh(55,0,4) && !invalidates && !drops);
 map_fresh_mark(55,0,4,before);assert(!efs_client_map_fresh(55,0,4));
 now_us++;map_fresh_mark(55,0,4,now_us);now_us++;assert(efs_client_map_fresh(55,0,4));
 inode.size=300000;invalidate_file_layout(&inode,0);
 assert(!efs_client_map_fresh(55,0,4) && invalidates==3 && !drops);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-map-fresh-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('map freshness: TTL, inode and chunk tags, close-to-open / metadata-change epoch, late pulls and zero-size fence PASS')
