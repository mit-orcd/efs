#!/usr/bin/env python3
"""Execute the actual local rebuild dispatch with controlled authority errors.

--source selects the old raft_host.c for the distinguishing negative control.
No daemon or storage fixture is started. The wire validator has its separate
test_put_upload_server coverage; this verifies the local caller reaches it.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source', type=Path, default=ROOT/'src/server/raft_host.c')
    p.add_argument('--sanitize', action='store_true')
    args = p.parse_args()
    source = args.source.read_text()
    start = source.index('static int dg_put_fragment(')
    branch = source.index('    if (id == h->s->id) {', start)
    pos = branch + source[branch:].index('{') + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    function = source[start:pos] + '\nreturn -99;\n}\n'
    prelude = r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#define EFS_OK 0
#define EFS_ERR_IO -1
#define EFS_ERR_FENCED -27
#define EFS_REBUILD_PUT_HEADER_LEN 142
typedef uint32_t efs_node_id_t;
struct efsd_server {uint32_t id;};
struct efs_raft_host {struct efsd_server *s;};
struct efs_export {uint32_t id;};
struct efs_node {int unused;};
struct efs_meta_degraded {uint32_t nodes[3];uint64_t ino,object_gen;
 uint32_t chunk_index;uint8_t checksums[3][32];};
struct efs_store {int unused;};struct efs_nvme_store {int unused;};
struct efs_frag_id {uint32_t export_id;uint64_t ino;uint32_t chunk_index,
 fragment_index;uint64_t chunk_generation;};
static int authority,checked,stored,synced;
static const uint8_t payload[4]={1,2,3,4};
static void wr32be(uint8_t *p,uint32_t n){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static void server_degraded_mark_encode(uint8_t *p,const struct efs_meta_degraded *d)
{memset(p,0,138);p[0]=(uint8_t)d->ino;}
static int server_rebuild_put_parts(struct efsd_server *s,const uint8_t *h,
 const uint8_t *data,uint32_t len,uint8_t *path)
{assert(s->id==1 && h[3]==1 && h[4]==9 && len==4 && data==payload);
 checked++;if(authority)return authority;stored++;*path=0;return 0;}
/* Old bypass remains compilable, so its missing authority check is observed. */
static void efs_store_nvme_bind(struct efs_store *a,struct efs_nvme_store *b,
 struct efsd_server *s,struct efs_export *e){(void)a;(void)b;(void)s;(void)e;}
static int efs_store_put(struct efs_store *s,const struct efs_frag_id *f,
 const uint8_t *p,uint32_t n,const uint8_t *sum)
{(void)s;(void)f;(void)p;(void)n;(void)sum;stored++;return 0;}
static int server_put_sync_commit(struct efsd_server *s,uint32_t *roots)
{(void)s;*roots=1;synced++;return 0;}
/* K110 tail of the devel function: only reached when the commit fails. */
static int server_put_sync_settled(void){return 1;}
'''
    checks = r'''
int main(void){struct efsd_server s={1};struct efs_raft_host h={&s};
 struct efs_export e={1};struct efs_meta_degraded d={.nodes={1,2,3},.ino=9};
 const int errors[]={-1,-9,-13,-14};
 for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++){
 authority=errors[i];checked=stored=synced=0;
 assert(dg_put_fragment(&h,&e,0,0,&d,0,payload,4)==authority);
 assert(checked==1 && !stored && !synced);}
 authority=0;checked=stored=synced=0;
 assert(!dg_put_fragment(&h,&e,0,0,&d,0,payload,4));
 assert(checked==1 && stored==1 && synced==1);return 0;}
'''
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp)/'control.c'
        c.write_text(prelude+function+checks)
        binary = Path(tmp)/'control'
        flags = ['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run(['cc','-std=c11','-O1',*flags,str(c),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('local rebuild: fresh authority before store, exact refusal, success barrier PASS')


if __name__ == '__main__':
    main()
