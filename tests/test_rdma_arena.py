#!/usr/bin/env python3
"""Registered transport buffers must release mappings on connection teardown."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/common/rdma.c').read_text()
a=s.index('static int conn_arena_create(');b=s.index('\nstatic struct efs_rdma_conn *conn_create(',a)
functions=s[a:b]
code=r'''
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct efs_rdma_conn {uint8_t *arena;size_t arena_bytes;};
static unsigned destroyed;
#ifndef NEGATIVE
static int tracked_unmap(void *p,size_t n){int rc=munmap(p,n);if(!rc)destroyed++;return rc;}
#define munmap tracked_unmap
#endif
#ifdef NEGATIVE
static int no_unmap(void *p,size_t n){(void)p;(void)n;return 0;}
#define munmap no_unmap
#endif
'''+functions+r'''
int main(void){
 struct efs_rdma_conn c[12]={{0}};
 size_t page=(size_t)sysconf(_SC_PAGESIZE),bytes=page*806;
 if(conn_arena_create(&c[0],0)!=-1 || c[0].arena || c[0].arena_bytes)return 2;
 conn_arena_destroy(&c[0]);
 for(unsigned round=0;round<8;round++){
  for(unsigned i=0;i<12;i++){
   if(conn_arena_create(&c[i],bytes) || (uintptr_t)c[i].arena%page || c[i].arena_bytes!=bytes)return 3;
   for(size_t off=0;off<bytes;off+=page)c[i].arena[off]=(uint8_t)(i+round);
  }
  for(unsigned i=0;i<12;i++){
   for(size_t off=0;off<bytes;off+=page)if(c[i].arena[off]!=(uint8_t)(i+round))return 4;
   void *old=c[i].arena;
   (void)old;
   conn_arena_destroy(&c[i]);
   if(c[i].arena || c[i].arena_bytes)return 5;
   if(destroyed!=round*12+i+1){fputs("closed connection mapping retained\n",stderr);return 6;}
#ifdef __linux__
   unsigned char vec;errno=0;
   if(mincore(old,page,&vec)!=-1 || errno!=ENOMEM)return 7;
#endif
   conn_arena_destroy(&c[i]);
  }
 }
 puts("RDMA arena: page alignment, live page patterns, 96 releases, failed allocation and double teardown PASS");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-rdma-arena-',dir=os.environ.get('TMPDIR')) as d:
 p=Path(d);(p/'case.c').write_text(code)
 for mode in ('fixed','negative'):
  subprocess.run([os.environ.get('CC','cc'),'-O2','-Wall','-Wextra','-Werror',*(['-DNEGATIVE'] if mode=='negative' else []),str(p/'case.c'),'-o',str(p/mode)],check=True)
  r=subprocess.run([str(p/mode)],text=True,capture_output=True,timeout=30)
  assert r.returncode==(6 if mode=='negative' else 0),r.stdout+r.stderr
  print(mode+': '+(r.stdout+r.stderr).strip())
# The full owner still must destroy QP/MRs before unmapping registered pages.
destroy=s[s.index('void efs_rdma_conn_destroy('):s.index('uint32_t efs_rdma_max_frame(')]
assert destroy.index('ibv_dereg_mr(rc->mr)')<destroy.index('conn_arena_destroy(rc)')
assert 'conn_arena_create(rc, total)' in s
