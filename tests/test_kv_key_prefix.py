#!/usr/bin/env python3
"""Exact ordered prefixes, short/unaligned bounds and sanitizers on real code."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=root/'src/kv/kv_lsm.c')
p.add_argument('--output', type=Path)
a = p.parse_args()
s = a.source.read_text()
body = s[s.index('static __uint128_t key_pfx'):s.index('/* Index of key')]
code = r'''
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
''' + body + r'''
static __uint128_t oracle(const uint8_t *p,unsigned n) {
 __uint128_t v=0;
 for(unsigned i=0;i<16;i++)v=v*256+(i<n?p[i]:0);
 return v;
}
int main(void) {
 uint32_t state=17;assert(key_pfx(NULL,0)==0);
 for(unsigned n=1;n<=64;n++)for(unsigned offset=0;offset<16;offset++) {
  uint8_t *allocation=malloc(n+offset);assert(allocation);
  uint8_t *key=allocation+offset;
  for(unsigned round=0;round<100;round++) {
   for(unsigned i=0;i<n;i++){state=state*1664525+1013904223;key[i]=state>>24;}
   assert(key_pfx(key,n)==oracle(key,n));
  }
  free(allocation);
 }
 puts("prefix: NULL empty, 1..64 exact allocations, all alignments, independent byte oracle PASS");
}
'''
def run(d):
    c=d/'prefix.c';c.write_text(code)
    for label,flags in [('native',[]),('san',['-fsanitize=address,undefined','-fno-omit-frame-pointer'])]:
        binary=d/label
        subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-O2','-Wall','-Wextra','-Werror']+flags+[str(c),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
if a.output:
    a.output.mkdir(exist_ok=False);run(a.output)
else:
    with tempfile.TemporaryDirectory(prefix='efs-prefix-') as d:run(Path(d))
