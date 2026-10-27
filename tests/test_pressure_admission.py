#!/usr/bin/env python3
"""Production admission preserves credits and fallback when background is unavailable."""
from pathlib import Path
import os,shlex,subprocess,tempfile
s=Path(os.environ.get('PRESSURE_ADMISSION_SOURCE',Path(__file__).resolve().parents[1]/'src/client/efs_fuse.c')).read_text()
a=s.index('static int fuse_write_admit(size_t size)');b=s.index('\n}',a)+2
code=r'''
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#define EFS_OK 0
#define EFS_ERR_BUSY -13
#define EFS_ERR_NOMEM -4
#define EFS_ERR_QUOTA -5
#define EFS_CHUNK_SIZE 131072
#define EFS_DCACHE_ENTRY_BUDGET 1024
typedef uint64_t efs_ino_t;
static int expired,available=1,credit,kicks,worker=1,high=1,drains,drain_rc,unreserves,cohorts_to_free=1,whole_drains;
static int efs_client_rpc_past_deadline(void){return expired;}
static uint64_t fuse_chunk_size(void){return EFS_CHUNK_SIZE;}
static uint64_t efs_client_rpc_deadline_ms(void){return 0;}
static uint64_t stats_now_ms(void){return 1;}
static int efs_buf_reserve_request(uint64_t b,uint64_t m){assert(b&&m&&!credit);if(!available)return EFS_ERR_BUSY;credit=1;return 0;}
static void efs_buf_unreserve(void){assert(credit);credit=0;unreserves++;}
static int efs_buf_should_drain(void){return high;}
static int fuse_pressure_kick(void){kicks++;return worker;}
static void efs_buf_budget_stats(uint64_t*a,uint64_t*b,uint64_t*c,uint64_t*d){*a=*b=*c=*d=0;}
static void efs_buf_metadata_stats(uint64_t*a,uint64_t*b,uint64_t*c){*a=*b=*c=0;}
static void efs_rdcache_trim(void){}
static void efs_dcache_trim_metadata(void){}
static efs_ino_t efs_dcache_pressure_ino(efs_ino_t after){return after?0:42;}
static void efs_buf_drain_enter(void){}
static void efs_buf_drain_leave(void){}
static int __attribute__((unused)) efs_append_flush_report(void*f,efs_ino_t ino){(void)f;assert(ino==42);whole_drains++;return EFS_ERR_BUSY;}
static int __attribute__((unused)) fuse_pressure_flush(efs_ino_t ino){assert(ino==42);drains++;if(!drain_rc&&!--cohorts_to_free){available=1;high=0;}return drain_rc;}
'''+s[a:b]+r'''
int main(void){
assert(!fuse_write_admit(1048576)&&credit&&kicks==1&&!drains&&!unreserves);
efs_buf_unreserve();worker=0;
assert(!fuse_write_admit(1048576)&&credit&&drains==1);efs_buf_unreserve();
available=0;high=1;worker=1;drain_rc=EFS_ERR_QUOTA;
assert(fuse_write_admit(1048576)==-ENOSPC&&!credit&&drains==2);
drain_rc=EFS_ERR_BUSY;assert(fuse_write_admit(1048576)==-EAGAIN&&!credit);
drain_rc=-99;assert(fuse_write_admit(1048576)==-EIO&&!credit);
drain_rc=0;cohorts_to_free=1;assert(!fuse_write_admit(1048576)&&credit);efs_buf_unreserve();
available=0;cohorts_to_free=3;int before=drains;
assert(!fuse_write_admit(1048576)&&credit&&drains==before+3&&!whole_drains);efs_buf_unreserve();
expired=1;assert(fuse_write_admit(1)==-EAGAIN&&!credit);
puts("pressure admission: reserved success, worker failure fallback, real budget refusal, error propagation, deadline PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pressure-admission-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+shlex.split(os.environ.get('EXTRA_CFLAGS',''))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=15)
