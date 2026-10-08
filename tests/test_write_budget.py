#!/usr/bin/env python3
"""Both production FUSE write entry points scope one inherited call budget."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/efs_fuse.c').read_text()
def fn(name):
 m=re.search(r'^static int '+name+r'\([^;]+?\)\n\{',s,re.M)
 return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
struct fuse_file_info {int flags;};struct fuse_bufvec {int unused;};
static uint64_t deadline,seen,now=1000;static int calls,write_result=-EIO,sync_calls,sync_result,sync_data;
static int efs_dcache_trace_on(void) {return 0;}
static uint64_t efs_file_ino(struct fuse_file_info *fi,int unused) {(void)fi;(void)unused;return 42;}
static int efs_fuse_fsync_ino(int ino,int data,struct fuse_file_info *fi) {(void)ino;(void)fi;assert(deadline==seen);sync_calls++;sync_data=data;return sync_result;}
static uint64_t stats_now_ms(void) {return now;}
static uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
static void efs_client_rpc_set_deadline_ms(uint64_t d) {deadline=d;}
static int efs_client_rpc_past_deadline(void) {return deadline&&now>=deadline;}
static int efs_fuse_write_run(const char *p,const char *b,size_t n,off_t o,struct fuse_file_info *f) {(void)p;(void)b;(void)n;(void)o;(void)f;seen=deadline;calls++;return write_result;}
static int efs_fuse_write_buf_run(const char *p,struct fuse_bufvec *b,off_t o,struct fuse_file_info *f) {(void)p;(void)b;(void)o;(void)f;seen=deadline;calls++;return write_result;}
'''+fn('efs_fuse_sync_write_result')+fn('efs_fuse_write')+fn('efs_fuse_write_buf')+r'''
int main(void) {
 assert(efs_fuse_write(NULL,NULL,1,0,NULL)==-EIO&&seen==31000&&!deadline);
 deadline=1100;assert(efs_fuse_write_buf(NULL,NULL,0,NULL)==-EIO&&seen==1100&&deadline==1100);
 deadline=1000;assert(efs_fuse_write(NULL,NULL,1,0,NULL)==-EAGAIN&&deadline==1000&&calls==2);
 assert(efs_fuse_write_buf(NULL,NULL,0,NULL)==-EAGAIN&&deadline==1000&&calls==2);
 deadline=0;write_result=4096;struct fuse_file_info fi={0};
 assert(efs_fuse_write(NULL,NULL,4096,0,&fi)==4096 && !sync_calls && !deadline);
 fi.flags=O_DSYNC;
 assert(efs_fuse_write(NULL,NULL,4096,0,&fi)==4096 && sync_calls==1 && sync_data && !deadline);
 fi.flags=O_SYNC|O_APPEND;
 assert(efs_fuse_write_buf(NULL,NULL,0,&fi)==4096 && sync_calls==2 && !sync_data && !deadline);
 sync_result=-EIO;
 assert(efs_fuse_write(NULL,NULL,4096,0,&fi)==-EIO && sync_calls==3 && !deadline);
 sync_result=0;write_result=-ENOSPC;
 assert(efs_fuse_write_buf(NULL,NULL,0,&fi)==-ENOSPC && sync_calls==3);
 write_result=0;
 assert(efs_fuse_write(NULL,NULL,0,0,&fi)==0 && sync_calls==3);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-write-budget-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('write call budgets: both entries, nested restoration, synchronous publication/failure and expired refusal PASS')
