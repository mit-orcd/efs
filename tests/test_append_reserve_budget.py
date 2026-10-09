#!/usr/bin/env python3
"""The actual reservation loop keeps one identity and bounds sleep/relock."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/efs_fuse.c').read_text()
def fn(name):
 m=re.search(r'^static int '+name+r'\([^;]+?\)\n\{',s,re.M)
 return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/types.h>
#define EFS_OK 0
#define EFS_ERR_BUSY -1
#define EFS_ERR_IO -2
#define EFS_CHUNK_SIZE 131072
typedef uint64_t efs_ino_t;
struct efs_opid_req {struct {uint64_t seq;} id;};
struct efs_inode {uint64_t size;};
struct test_export {uint32_t chunk_size;};
static struct {struct test_export export;uint32_t export_id;pthread_mutex_t idx_mu;} g_client={.idx_mu=PTHREAD_MUTEX_INITIALIZER};
static pthread_mutex_t stripe=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_open_mu=PTHREAD_MUTEX_INITIALIZER;
struct efs_open_ref {efs_ino_t ino;uint32_t append_burst;uint64_t append_burst_ms,append_handoff_ms;int append_publishing;struct efs_open_ref *next;};
static struct efs_open_ref open_ref={.ino=1},*g_open_refs=&open_ref;
static uint64_t cached_size;
static int flush_calls,flush_rc,clear_publish_on_sleep;
static uint64_t now,deadline;static unsigned calls,begins,ends,sleeps,last_sleep;
static int accept_after,rpc_expires,contend,begin_failure,extent_failure,other_held;
static pthread_mutex_t *append_mu(efs_ino_t ino) {(void)ino;return &stripe;}
static uint64_t stats_now_ms(void) {return now;}
static uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
static int efs_client_rpc_past_deadline(void) {return deadline && now>=deadline;}
static void usleep(unsigned micros) {
 if(other_held)assert(pthread_mutex_trylock(&stripe)==EBUSY);
 else {assert(!pthread_mutex_trylock(&stripe));pthread_mutex_unlock(&stripe);}
 sleeps++;last_sleep=micros;now+=(micros+999)/1000;
 if(clear_publish_on_sleep){open_ref.append_publishing=0;clear_publish_on_sleep=0;}
 if(contend) {assert(!pthread_mutex_lock(&stripe));contend=0;other_held=1;}
}
static int efs_client_append_begin(struct efs_opid_req *q) {begins++;q->id.seq=42;return begin_failure?-1:7;}
static void efs_client_append_end(int slot) {assert(slot==7);ends++;}
static int efs_client_rpc_append_reserve(uint32_t export,efs_ino_t ino,uint64_t len,const struct efs_opid_req *q,uint64_t *end,uint64_t *start) {
 (void)export;(void)ino;assert(q->id.seq==42);calls++;
 if(rpc_expires)now=deadline;
 if(accept_after && calls==(unsigned)accept_after){*start=100;*end=100+len;return EFS_OK;}
 return EFS_ERR_BUSY;
}
static int efs_dcache_trace_on(void) {return 0;}
static int efs_write_extent_valid(uint64_t off,uint64_t len,uint32_t cs) {(void)off;(void)len;(void)cs;return extent_failure;}
static void efs_client_stage_pin(efs_ino_t ino) {(void)ino;}
static void efs_rdcache_invalidate(efs_ino_t ino,uint32_t ci) {(void)ino;(void)ci;}
static void efs_client_lock_dir(efs_ino_t ino) {(void)ino;}
static void efs_client_unlock_dir(efs_ino_t ino) {(void)ino;}
static int efs_export_get_inode(struct test_export *e,efs_ino_t ino,struct efs_inode *row) {(void)e;(void)ino;row->size=cached_size;return 0;}
static void efs_export_set_size(struct test_export *e,efs_ino_t ino,uint64_t size) {(void)e;(void)ino;cached_size=size;}
static int efs_append_flush_report(void *fi,efs_ino_t ino) {(void)fi;(void)ino;flush_calls++;assert(open_ref.append_publishing);assert(!pthread_mutex_trylock(&stripe));pthread_mutex_unlock(&stripe);return flush_rc;}
'''+fn('wb_lock_budget')+fn('append_retry_wait')+fn('append_publication_wait')+fn('append_reserve_offset')+fn('append_publish_burst')+r'''
static void reset(void) {
 now=1000;deadline=1100;calls=begins=ends=sleeps=last_sleep=0;
 accept_after=rpc_expires=contend=begin_failure=extent_failure=other_held=0;
 cached_size=flush_calls=flush_rc=clear_publish_on_sleep=0;open_ref=(struct efs_open_ref){.ino=1};
 assert(!pthread_mutex_lock(&stripe));
}
static void free_stripe(void) {assert(!pthread_mutex_trylock(&stripe));pthread_mutex_unlock(&stripe);}
int main(void) {
 off_t off=0;
 reset();deadline=1000;
 assert(append_reserve_offset(1,8,&off)==-EIO && !calls && begins==1 && ends==1);free_stripe();
 reset();rpc_expires=1;
 assert(append_reserve_offset(1,8,&off)==-EIO && calls==1 && !sleeps && ends==1);free_stripe();
 reset();deadline=1001;
 assert(append_reserve_offset(1,8,&off)==-EIO && calls==1 && sleeps==1 && last_sleep==1000 && now==1001);free_stripe();
 reset();deadline=1005;contend=1;
 assert(append_reserve_offset(1,8,&off)==-EIO && calls==1 && now==1005 && ends==1);
 assert(pthread_mutex_trylock(&stripe)==EBUSY);pthread_mutex_unlock(&stripe);
 reset();accept_after=3;
 assert(!append_reserve_offset(1,8,&off) && off==100 && calls==3 && begins==1 && ends==1);
 assert(pthread_mutex_trylock(&stripe)==EBUSY);pthread_mutex_unlock(&stripe);
 reset();begin_failure=1;assert(append_reserve_offset(1,8,&off)==-EAGAIN && !calls && !ends);free_stripe();
 reset();accept_after=1;extent_failure=1;
 assert(append_reserve_offset(1,8,&off)==-EFBIG && calls==1 && ends==1);free_stripe();
 reset();accept_after=1;cached_size=1000;
 assert(!append_reserve_offset(1,8,&off) && cached_size==108);pthread_mutex_unlock(&stripe);
 reset();open_ref.append_handoff_ms=1003;accept_after=1;
 assert(!append_reserve_offset(1,8,&off) && calls==1 && now==1003);pthread_mutex_unlock(&stripe);
 reset();open_ref.append_publishing=1;deadline=1005;
 assert(append_reserve_offset(1,8,&off)==-EIO && !calls && ends==1 && now==1005 && open_ref.append_publishing);free_stripe();
 reset();open_ref.append_publishing=1;clear_publish_on_sleep=1;accept_after=1;
 assert(!append_reserve_offset(1,8,&off) && calls==1 && sleeps==1 && !open_ref.append_publishing);pthread_mutex_unlock(&stripe);
 reset();pthread_mutex_unlock(&stripe);open_ref.append_burst=32;
 assert(append_publish_burst(1)==EFS_OK && flush_calls==1 && !open_ref.append_publishing && open_ref.append_handoff_ms==1100);free_stripe();
 reset();pthread_mutex_unlock(&stripe);open_ref.append_burst=32;flush_rc=EFS_ERR_IO;
 assert(append_publish_burst(1)==EFS_ERR_IO && flush_calls==1 && !open_ref.append_publishing && !open_ref.append_handoff_ms);free_stripe();
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-budget-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('append reservation: identity/deadline/stripe ownership, lower authoritative EOF, publication failure and bounded all-writer handoff PASS')
