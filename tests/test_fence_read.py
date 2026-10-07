#!/usr/bin/env python3
"""Exercise production captured-view reads with deterministic object/GC stubs."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
r=(root/'src/client/read.c').read_text()
def function(name):
    m=re.search(r'^(?:static )?[^\n;]*\b'+name+r'\([^;]+?\)\n\{',r,re.M)
    return r[m.start():r.index('\n}',m.end())+2]
source=r'''
#include "client_internal.h"
#include "efs/checksum.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#define CS EFS_DEFAULT_CHUNK_SIZE
struct efs_client g_client;
static struct efs_chunk_entry map;
static int decodes, overlays, decode_error, pulls, pull_error;
static uint32_t data_chunk_size(void) { return CS; }
static uint32_t data_frag_size(void) { return CS/2; }
int efs_export_get_chunk(struct efs_export *ex, efs_ino_t ino,
                         uint32_t ci, struct efs_chunk_entry *out) {
    (void)ex;(void)ino;(void)ci; if(out)*out=map; return EFS_OK;
}
void efs_hash_zero_fragment_len(size_t n,uint8_t out[EFS_HASH_SIZE]) {
    (void)n; memset(out,0xff,EFS_HASH_SIZE);
}
void efs_hash_zero_fragment(uint8_t out[EFS_HASH_SIZE]) { memset(out,0xff,EFS_HASH_SIZE); }
int efs_bytes_are_zero(const void *p,size_t n) {
    const uint8_t *b=p; for(size_t i=0;i<n;i++) if(b[i]) return 0; return 1;
}
static int efs_client_decode_placed_chunk_attempts(efs_ino_t ino,uint32_t ci,
    uint8_t *out,uint32_t cs,uint32_t fl,int attempts,int hole,
    const struct efs_chunk_entry *ce) {
    (void)ino;(void)ci;(void)fl;(void)attempts;(void)hole;(void)ce;
    decodes++; if(decode_error) return decode_error;
    memset(out,'A',cs);return EFS_OK;
}
static int overlay_one_delta(efs_ino_t ino,uint32_t ci,uint8_t *out,
    uint32_t len,const struct efs_chunk_delta *d) {
    (void)ino;(void)ci; assert(d->off+d->len<=len);
    overlays++;memset(out+d->off,(int)d->generation,d->len);return EFS_OK;
}
int efs_client_pull_chunks_range(efs_ino_t ino,uint32_t a,uint32_t b) {
    (void)ino;(void)a;(void)b;pulls++; if(pull_error)return pull_error;
    decode_error=0;map.read_view.parts[0].len=50;map.read_view.revision++;
    return EFS_OK;
}
'''+function('overlay_chunk_deltas_ce')+'\n'+function('fetch_published_once')+'\n'+function('efs_client_fetch_published_chunk_obs')+r'''
#define RDCACHE_WAYS 2
#define RDCACHE_STRIPES 1
struct rdcache_ent {
    efs_ino_t ino; uint32_t ci; uint8_t *data; uint32_t len,tick;
    uint64_t gen; int pending,pins;
};
static struct { struct rdcache_ent e[1][RDCACHE_WAYS];
    uint32_t tick; pthread_cond_t cv[1]; } g_rdcache;
static pthread_mutex_t cache_mu=PTHREAD_MUTEX_INITIALIZER;
static uint32_t rdcache_slot(efs_ino_t ino,uint32_t ci) {(void)ino;(void)ci;return 0;}
static pthread_mutex_t *rdcache_mu(uint32_t s) {(void)s;return &cache_mu;}
void *efs_buf_alloc(uint32_t len) {return malloc(len);}
void *efs_buf_alloc_prefetch(uint32_t len) {return efs_buf_alloc(len);}
void efs_buf_free(void *p,uint32_t len) {(void)len;free(p);}
'''+function('rdcache_map_gen')+'\n'+function('rdcache_put_inner')+r'''
int main(void) {
    assert(!pthread_mutex_init(&g_client.idx_mu,NULL));
    uint8_t *buf=malloc(CS);assert(buf);
    map.generation=11; memset(map.checksums,1,sizeof(map.checksums));
    map.ndelta=2;map.delta_seq=9;
    map.deltas[0].off=500;map.deltas[0].len=100;map.deltas[0].generation='B';
    map.deltas[1].off=650;map.deltas[1].len=100;map.deltas[1].generation='C';
    map.read_view=(struct efs_fence_view){.revision=2,.chunk_size=CS,.count=3};
    map.read_view.parts[0]=(struct efs_fence_part){0,0,100};
    map.read_view.parts[1]=(struct efs_fence_part){1,500,100};
    map.read_view.parts[2]=(struct efs_fence_part){1,650,50};
    struct efs_chunk_entry obs;int have=0;
    assert(!fetch_published_once(1,0,buf,CS,&obs,&have));assert(have);
    for(uint32_t i=0;i<CS;i++)
        assert(buf[i]==(i<100?'A':i>=500&&i<600?'B':i>=650&&i<700?'C':0));
    assert(decodes==1&&overlays==2);
    uint64_t key=efs_chunk_read_key(&map);
    map.read_view.fence_epoch=1;
    assert(key!=efs_chunk_read_key(&map));
    key=efs_chunk_read_key(&map);
    map.read_view.parts[2].len=0;
    assert(key!=efs_chunk_read_key(&map));
    key=efs_chunk_read_key(&map);map.read_view.revision++;
    assert(key!=efs_chunk_read_key(&map));
    map.read_view.parts[0].len=0;map.read_view.parts[1].len=0;
    decodes=overlays=0;decode_error=EFS_ERR_NOT_FOUND;
    assert(!fetch_published_once(1,0,buf,CS,&obs,&have));
    assert(!decodes&&!overlays);for(uint32_t i=0;i<CS;i++)assert(!buf[i]);
    map.read_view.parts[0].len=100;
    assert(!efs_client_fetch_published_chunk_obs(1,0,buf,CS,&obs,&have));
    assert(pulls==1&&obs.read_view.parts[0].len==50);
    decode_error=EFS_ERR_NOT_FOUND;pull_error=EFS_ERR_BUSY;
    assert(efs_client_fetch_published_chunk_obs(1,0,buf,CS,&obs,&have)==EFS_ERR_BUSY);
    struct efs_chunk_rec rec={0},decoded;
    rec.chunk_generation=map.generation;rec.delta_base_n=map.ndelta;
    rec.publish_flags=EFS_CHUNK_REC_F_CAPTURED_EPOCH;rec.publish_epoch=1;
    rec.read_view=map.read_view;memcpy(rec.deltas,map.deltas,sizeof(rec.deltas));
    assert(!efs_chunk_rec_view_valid(&rec));
    uint8_t wire[sizeof(rec)];size_t used=0,n=efs_chunk_rec_pack(wire,sizeof(wire),&rec);
    assert(n && !efs_chunk_recs_unpack(wire,n,1,&decoded,&used)&&used==n);
    assert(!memcmp(&decoded.read_view,&rec.read_view,sizeof(rec.read_view)));
    assert(!efs_chunk_rec_view_valid(&decoded));
    rec.read_view.parts[1].off++;assert(efs_chunk_rec_view_valid(&rec)==EFS_ERR_PROTO);
    rec.publish_flags=EFS_CHUNK_REC_F_CAPTURED_EPOCH;rec.publish_epoch=1;
    rec.read_view=map.read_view;rec.read_view.count=EFS_FENCE_PART_MAX+1;
    assert(efs_chunk_rec_view_valid(&rec)==EFS_ERR_PROTO);
    /* A racing adoption must not relabel bytes fetched from the old view. */
    assert(!pthread_cond_init(&g_rdcache.cv[0],NULL));
    uint64_t captured_key=efs_chunk_read_key(&map);
    map.read_view.parts[0].len++;
    assert(captured_key!=rdcache_map_gen(1,0));
    assert(!rdcache_put_inner(1,0,buf,CS,0,captured_key));
    assert(g_rdcache.e[0][0].gen==captured_key);
    assert(g_rdcache.e[0][0].gen!=rdcache_map_gen(1,0));
    g_rdcache.e[0][0].pins=1;
    uint8_t *owned=malloc(CS);assert(owned);
    assert(!rdcache_put_inner(1,0,owned,CS,1,rdcache_map_gen(1,0)));
    free(owned);free(g_rdcache.e[0][0].data);
    free(buf);puts("fence reads: mixed ages, dead-object skips, refresh failure, view keys and wire validation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-fence-read-') as directory:
    p=Path(directory)/'test.c';p.write_text(source)
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client')]
    cmd+=shlex.split(os.environ.get('FENCE_READ_CFLAGS',''))
    subprocess.run(cmd+[str(p),str(root/'src/common/common.c'),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
