#!/usr/bin/env python3
"""Exercise the production span/parity fragment deadline scope."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/read.c').read_text()
start=s.index('int efs_client_get_fragment(')
body=s[start:s.index('\n}',start)+2]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
typedef uint32_t efs_node_id_t;
typedef uint64_t efs_ino_t;
#define EFS_HASH_SIZE 32
static int read_frag_timeout_ms=2000;
static uint64_t current, expected;
static int result;
static uint64_t read_peer_now_ms(void) { return 10000; }
static uint64_t efs_client_rpc_deadline_ms(void) { return current; }
static void efs_client_rpc_set_deadline_ms(uint64_t d) { current=d; }
static int get_fragment_run(efs_node_id_t n,efs_ino_t ino,uint32_t ci,
 uint32_t fi,uint32_t len,uint8_t *data,uint32_t *out,uint8_t *sum,uint64_t gen) {
 assert(current==expected);
 assert(n==2 && ino==3 && ci==4 && fi==1 && len==65536 && gen==5);
 assert(data && out && sum);
 return result;
}
''' + body + r'''
int main(void) {
 uint8_t data[1],sum[32];uint32_t len=0;
 uint64_t parents[]={0,15000,11000,9999};
 for(unsigned i=0;i<4;i++) for(int r=0;r<3;r++) {
  current=parents[i];expected=current && current<12000?current:12000;
  result=r==0?0:r==1?-6:-13;
  assert(efs_client_get_fragment(2,3,4,1,65536,data,&len,sum,5)==result);
  assert(current==parents[i]);
 }
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run([os.environ.get('CC','cc'),'-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('fragment attempt parent bound and restoration: PASS')
