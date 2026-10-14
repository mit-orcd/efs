#!/usr/bin/env python3
"""Two REPORT/fsync attempts must not adopt an unvalidated committed EOF.

Executes production repair and subsequent snapshot blocks. RPC/adoption and
local lookup are adapters; this does not simulate the server's publication.
Set W100_SOURCE_REF=c3d8ee560 for the original-code negative control.
"""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
ref=os.environ.get('W100_SOURCE_REF')
source_file=os.environ.get('W100_SOURCE_FILE')
s=Path(source_file).read_text() if source_file else (subprocess.check_output(['git','show',ref+':src/client/write.c'],text=True,cwd=root) if ref else (root/'src/client/write.c').read_text())
a=s.index('    for (uint32_t r = 0; r < n_repair; r++)');b=s.index('    /* A flush of a clean fd',a);repair=s[a:b]
a=s.index('    for (uint64_t i = 0; i <= ds.ino_mask; i++)',s.index('static int report_dirty_ino_run('));b=s.index('    /* Every chunk rec travels',a);snapshot=s[a:b]
code=r'''
#include "efs/protocol.h"
#include <assert.h>
#include <stdio.h>
struct {struct efs_export export;efs_export_id_t export_id;} g_client;
static int present,rpc_error;
static uint64_t server_eof;
static struct efs_inode staged;
int efs_export_get_inode(struct efs_export *ex,efs_ino_t ino,struct efs_inode *out){(void)ex;(void)ino;if(!present)return -1;*out=staged;return 0;}
int efs_client_rpc_getattr(efs_export_id_t ex,efs_ino_t ino,struct efs_inode *out){(void)ex;if(rpc_error)return rpc_error;*out=(struct efs_inode){.ino=ino,.size=server_eof};return EFS_OK;}
/* Match stat_ino's real adopt-on-miss side effect. */
static int __attribute__((unused)) efs_client_stat_ino(efs_ino_t ino,struct efs_inode *out){int rc=efs_client_rpc_getattr(0,ino,out);if(!rc){staged=*out;present=1;}return rc;}
static int efs_client_mtime_is_pinned(efs_ino_t ino){(void)ino;return 0;}
static unsigned data_chunk_size(void){return 131072;}
static unsigned attempt(uint64_t expected) {
struct efs_chunk_rec crecs[1]={{.ino=42,.chunk_index=64}};
struct efs_ino_size_rec irecs[1]={{0}};
efs_ino_t repair[1]={42};uint32_t n_repair=1,cn=1,in=0;
/* Ordinary retry snapshot runs before repair. */
uint64_t keys[1]={42};struct {uint64_t ino_mask,*ino_keys;} ds={0,keys};efs_ino_t only_ino=42;
'''+snapshot+r'''
if(!in) {
'''+repair+r'''
}
if(in)assert(irecs[0].size==expected);
return in;
}
int main(void){
/* First fsync refusal must not poison the second snapshot with server EOF0. */
server_eof=0;assert(!attempt(8388609));assert(!attempt(8388609));assert(!present);
server_eof=1048577;assert(!attempt(8388609));assert(!attempt(8388609));assert(!present);
/* Transient RPC failure also leaves ownership/EOF unresolved. */
rpc_error=EFS_ERR_BUSY;assert(!attempt(8388609));assert(!present);rpc_error=0;
/* A trustworthy local row restored by accepted-write ownership can retry
 * with its exact partial-chunk EOF, rather than a rounded coverage bound. */
staged=(struct efs_inode){.ino=42,.size=8388609};present=1;assert(attempt(8388609)==1);
/* Safe server coverage may be attached without changing local staging. */
present=0;server_eof=8519680;assert(attempt(8519680)==1);assert(!present);
puts("two-fsync EOF: stale0/low rows never adopted, repeat refusal, RPC failure, exact local recovery and covering remote row PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-retry-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
