#!/usr/bin/env python3
"""Isolate committed memtable insertion movement; not a daemon throughput gate."""
import argparse, os, shlex, subprocess, tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--keys',type=int,default=20000)
p.add_argument('--batches',type=int,default=1000)
p.add_argument('--batch-size',type=int,default=16)
p.add_argument('--repeats',type=int,default=5)
a=p.parse_args()
assert 0<a.keys<1<<24 and 0<a.batches*a.batch_size<1<<24
assert 0<a.batch_size<=32768 and 0<a.repeats<=100
root=Path(__file__).resolve().parents[2]
source=(root/'src/kv/kv_lsm.c').read_text()
section=source.split('/* --- memtable ',1)[1].split('/* --- live set ',1)[0]
section=section[section.index('static void ent_free'):]
main=r'''
#include <time.h>
static double now(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static int measure(int mode,unsigned repeat) {
 struct kv_mtab m={0};uint8_t key[4];
 for(unsigned i=0;i<KEYS;i++) {
  key[0]=1;key[1]=i>>16;key[2]=i>>8;key[3]=i;
  if(kv_mtab_set(&m,KV_OP_PUT,key,4,NULL,0))return 1;
 }
 double start=now();
 for(unsigned r=0;r<BATCHES;r++) {
  uint8_t keys[BATCH_SIZE][4];struct efs_kv_item it[BATCH_SIZE];
  for(unsigned i=0;i<BATCH_SIZE;i++) {
   unsigned k=r*BATCH_SIZE+i;keys[i][0]=0;keys[i][1]=k>>16;keys[i][2]=k>>8;keys[i][3]=k;
   it[i]=(struct efs_kv_item){EFS_KV_PUT,keys[i],4,NULL,0};
  }
  if(mode) {if(kv_mtab_batch(&m,it,BATCH_SIZE))return 2;}
  else for(unsigned i=0;i<BATCH_SIZE;i++)if(kv_mtab_set(&m,KV_OP_PUT,it[i].key,4,NULL,0))return 3;
 }
 double elapsed=now()-start;
 if(m.n!=KEYS+BATCHES*BATCH_SIZE)return 4;
 printf("%u\t%s\t%.9f\n",repeat,mode?"batch":"sequential",elapsed);
 kv_mtab_clear(&m);return 0;
}
int main(void) {
 puts("repeat\tmethod\tseconds");
 for(unsigned r=0;r<REPEATS;r++)for(unsigned i=0;i<2;i++) {
  int rc=measure((r+i)%2,r);if(rc)return rc;
 }
}
'''
with tempfile.TemporaryDirectory(prefix='efs-mtab-measure-') as d:
 c=Path(d)/'bench.c';binary=Path(d)/'bench'
 c.write_text('#include "kv_lsm_internal.h"\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'+section+main)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-O3','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/kv'),f'-DKEYS={a.keys}',f'-DBATCHES={a.batches}',f'-DBATCH_SIZE={a.batch_size}',f'-DREPEATS={a.repeats}',str(c),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
