#!/usr/bin/env python3
"""Exercise the mounted packed-read branch with the real aggregate body pool."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
read=(root/'src/client/read.c').read_text()
a=read.index('    if (have_ce && ce.packed.segment_id) {')
b=read.index('    int order[EFS_NUM_FRAGMENTS]',a)
source=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client_internal.h"
#include "efs/packed_reader.h"
#include "efs/checksum.h"
struct efs_client g_client;
int efs_rdma_zc_region_add(void *p,size_t n) {(void)p;(void)n;return 0;}
#include "bufpool.c"
static int expired,fetch_error,calls;
static uint8_t *frags[3];
int efs_client_rpc_past_deadline(void) {return expired;}
static int packed_fetch(void *ctx,efs_node_id_t node,uint64_t id,uint64_t gen,
 uint32_t fi,uint32_t geometry,uint8_t *out,uint32_t len) {
 (void)ctx;assert(node==fi+1 && id==41 && gen==43 && len==geometry/2);
 calls++;if(fetch_error)return fetch_error;memcpy(out,frags[fi],len);return 0;
}
static int mounted_read(efs_ino_t ino,uint32_t chunk_index,struct efs_chunk_entry ce,
 uint8_t *chunk_out,uint32_t chunk_size) {
 int have_ce=1;
'''+read[a:b]+r'''
 return EFS_ERR_INVAL;
}
int main(void) {
 setenv("EFS_DCACHE_HARD_BYTES","33554432",1);
 uint8_t payload[512]={97},image[EFS_SEGMENT_MAX_SIZE],output[EFS_MIN_CHUNK_SIZE],saved[EFS_MIN_CHUNK_SIZE];
 struct efs_pack_record rec={101,7,0,23,sizeof(payload),payload};
 struct efs_pack_ref ref;size_t geometry;
 assert(!efs_pack_build(41,43,0,&rec,1,image,sizeof(image),&geometry,&ref));
 struct efs_chunk_entry ce={.generation=23,.packed=ref,.fragment_nodes={1,2,3}};
 uint8_t *storage=malloc(geometry*3/2);assert(storage);
 for(unsigned i=0;i<3;i++)frags[i]=storage+geometry/2*i;
 assert(!efs_encode_segment(image,geometry,geometry,frags));
 for(unsigned i=0;i<3;i++)efs_hash(frags[i],geometry/2,ce.checksums[i]);
 void *held[256];for(unsigned i=0;i<256;i++){held[i]=efs_buf_alloc(EFS_CHUNK_SIZE);assert(held[i]);}
 memset(output,0x5a,sizeof(output));memcpy(saved,output,sizeof(output));
 uint64_t live=g_live;
 assert(mounted_read(101,0,ce,output,sizeof(output))==EFS_ERR_BUSY);
 assert(!calls && g_live==live && !memcmp(output,saved,sizeof(output)));
 /* Retained writer/cache bodies keep their charge while scratch is refused. */
 efs_buf_free(held[255],EFS_CHUNK_SIZE);live=g_live;
 expired=1;assert(mounted_read(101,0,ce,output,sizeof(output))==EFS_ERR_BUSY);
 assert(!calls && g_live==live);expired=0;
 fetch_error=EFS_ERR_BUSY;
 assert(mounted_read(101,0,ce,output,sizeof(output))==EFS_ERR_BUSY);
 assert(calls==1 && g_live==live && !memcmp(output,saved,sizeof(output)));
 fetch_error=0;
 assert(!mounted_read(101,0,ce,output,sizeof(output)));
 assert(g_live==live && !memcmp(output,payload,sizeof(payload)));
 for(unsigned i=sizeof(payload);i<sizeof(output);i++)assert(!output[i]);
 for(unsigned i=0;i<255;i++)efs_buf_free(held[i],EFS_CHUNK_SIZE);
 assert(!g_live);free(storage);
 puts("mounted packed-read budget: refusal before fetch, retained body charges, deadline, failed and successful scratch release PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-packed-budget-') as d:
 p=Path(d)/'t.c';p.write_text(source);exe=p.with_suffix('')
 sources=['src/common/packed_reader.c','src/common/packed_segment.c','src/common/common.c','src/common/checksum.c','src/data/erasure.c','deps/blake3/blake3.c','deps/blake3/blake3_portable.c','deps/blake3/blake3_dispatch.c']
 cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-D_GNU_SOURCE','-DBLAKE3_USE_NEON=0','-DBLAKE3_NO_SSE2','-DBLAKE3_NO_SSE41','-DBLAKE3_NO_AVX2','-DBLAKE3_NO_AVX512','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),'-I'+str(root/'deps/blake3'),str(p)]
 subprocess.run(cmd+[str(root/x) for x in sources]+['-pthread','-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
