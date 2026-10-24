#!/usr/bin/env python3
"""Actual marker helper: throttling, entry budget, sync errors and force drain."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
assert 'EFS_RAFT_BATCH_' not in s and 'host_batch_flag' not in s, 'D42: Raft batching has no switch'
def fn(name):
 a=s.index('static int '+name+'(')
 return s[a:s.index('\n}',a)+2]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "efs/common.h"
#define BO_COUNT(key) ((void)0)
#define HOST_APPLIED_MAX_TAIL 1024ull
struct group {int hosted;void*r;uint64_t applied_saved,applied_saved_us;int applied_fd;uint8_t group;uint64_t cursor_durable;};
struct efs_raft_host {struct group g[2];uint64_t applied_persist_us;};
struct host_view {int has;};
static void host_view_get(struct efs_raft_host *h,uint8_t group,struct host_view *v){v->has=h->g[group>>1].hosted && h->g[group>>1].r!=NULL;}
static int writes,syncs,fail_open,fail_write,fail_sync,closes;
static uint8_t record[16];
#define O_RDWR 2
#define O_CREAT 64
static int open(const char*p,int f,int mode){(void)p;(void)f;(void)mode;return fail_open?-1:3;}
static ssize_t pwrite(int fd,const void*p,size_t n,off_t o){(void)fd;(void)o;writes++;memcpy(record,p,n);return fail_write?0:(ssize_t)n;}
static int fdatasync(int fd){(void)fd;syncs++;return fail_sync?-1:0;}
static int close(int fd){(void)fd;closes++;return 0;}
static void applied_path(struct efs_raft_host*h,uint8_t g,char*p,size_t n){(void)h;(void)g;snprintf(p,n,"marker");}
static void wr64be(uint8_t*p,uint64_t v){for(int i=7;i>=0;i--){p[i]=v&255;v>>=8;}}
'''+s[s.index('#define HOST_APPLIED_PERSIST_US'):s.index('\n',s.index('#define HOST_APPLIED_PERSIST_US'))+1]+fn('host_applied_interval')+fn('persist_applied')+r'''
int main(void){
 uint64_t us;
 assert(host_applied_interval(NULL,&us)==EFS_OK&&us==100000); /* 100 ms default */
 const char*bad[]={"","0","9","1001","-10","+10","1e2","100ms"," 100","999999999999999999999"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(host_applied_interval(bad[i],&us)==EFS_ERR_INVAL);
 assert(host_applied_interval("1000",&us)==EFS_OK&&us==1000000);
 assert(host_applied_interval("100",&us)==EFS_OK&&us==100000);
 struct efs_raft_host h={0};h.applied_persist_us=us;h.g[0]=(struct group){1,&h,10,100000,3,0,0};
 assert(persist_applied(&h,0,11,150000,0)==EFS_OK&&!writes);
 assert(persist_applied(&h,0,12,200000,0)==EFS_OK&&writes==1&&syncs==1);
 assert(h.g[0].applied_saved==12&&h.g[0].applied_saved_us==200000);
 uint8_t want[16];wr64be(want,12);wr64be(want+8,~(uint64_t)12);assert(!memcmp(want,record,16));
 assert(persist_applied(&h,0,1036,200001,0)==EFS_OK&&writes==2); /* entry budget */
 assert(persist_applied(&h,0,1037,200002,1)==EFS_OK&&writes==3); /* shutdown bypass */
 for(int kind=0;kind<3;kind++){
  h.g[0].applied_fd=-1;fail_open=kind==0;fail_write=kind==1;fail_sync=kind==2;
  assert(persist_applied(&h,0,1038,400000,0)==EFS_ERR_IO);
  assert(h.g[0].applied_saved==1037&&h.g[0].applied_saved_us==200002);
  fail_open=fail_write=fail_sync=0;
  /* Retry within one microsecond, not a new cadence delay after failure. */
  assert(persist_applied(&h,0,1038,400001,0)==EFS_OK);
  assert(h.g[0].applied_saved==1038);h.g[0].applied_saved=1037;h.g[0].applied_saved_us=200002;
 }
 int before=writes;
 assert(persist_applied(&h,0,1037,900000,1)==EFS_OK&&writes==before);
 assert(persist_applied(&h,0,0,900000,1)==EFS_OK&&writes==before);
 /* A backward clock does not suppress a changed checkpoint. */
 assert(persist_applied(&h,0,1038,1,0)==EFS_OK&&writes==before+1);
 h.g[1].hosted=0;assert(persist_applied(&h,1,42,900000,1)==EFS_OK&&writes==before+1);
 /* Only a successfully flushed cursor can replace an external marker.
  * Config/no-op entries beyond it retain the original force/error behavior. */
 before=writes;h.g[0].cursor_durable=2000;
 assert(persist_applied(&h,0,2000,900000,1)==EFS_OK && writes==before);
 assert(h.g[0].applied_saved==2000 && h.g[0].applied_saved_us==900000);
 assert(persist_applied(&h,0,2001,900001,1)==EFS_OK && writes==before+1);
 fail_sync=1;assert(persist_applied(&h,0,2002,1000000,1)==EFS_ERR_IO);
 assert(h.g[0].applied_saved==2001);fail_sync=0;
 /* Do not use a newer proof to silently suppress a backwards checkpoint. */
 before=writes;h.g[0].cursor_durable=3000;
 assert(persist_applied(&h,0,1999,1100000,1)==EFS_OK && writes==before+1);

 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-applied-interval-') as d:
 p=Path(d)/'test.c';p.write_text(code);out=p.with_suffix('')
 flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('EFS_TEST_SANITIZE')=='1' else []
 subprocess.run([os.environ.get('CC','cc'),*flags,'-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(out)],check=True)
 subprocess.run([str(out)],check=True)
print('applied interval: bounded cadence/tail, durable record, immediate error retry, shutdown force PASS')
