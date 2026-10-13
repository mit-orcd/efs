#!/usr/bin/env python3
"""Run actual write acceptance and REPORT exclusion through an early reclaim."""
from pathlib import Path
import os,re,resource,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/write.c').read_text()
def function(name):
 m=re.search(r'^(?:static )?(?:int|void) '+name+r'\([^;]+?\)\n\{',s,re.M)
 assert m,name
 return s[m.start():s.index('\n}',m.end())+2]
a=s.index('#define PUB_SLOTS');b=s.index('/* Caller holds dirty_mu. */',a)
code=r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
typedef uint64_t efs_ino_t;
struct {pthread_mutex_t dirty_mu;} g_client={PTHREAD_MUTEX_INITIALIZER};
static uint64_t outer,logical_size,server_size;
static int bodies,published,fail_second;
static uint64_t report_clock_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static uint64_t efs_client_rpc_deadline_ms(void){return outer;}
static int dirty_set_has(uint64_t *keys,uint64_t mask,efs_ino_t ino){return keys&&mask==0&&keys[0]==ino;}
static void meta_flush_enqueue(efs_ino_t ino){assert(ino==42);published++;server_size=logical_size;}
'''+s[a:b]+'\n'+function('pub_has_locked')+'\n'+function('pub_take_locked')+'\n'+function('write_accept_begin')+'\n'+function('write_accept_end')+r'''
static uint32_t data_chunk_size(void){return 131072;}
#define EFS_OK 0
static int efs_write_extent_valid(uint64_t off,uint32_t len,uint32_t cs){return cs&&len&&off<=UINT64_MAX-len?0:-1;}
static void dcache_note_size(efs_ino_t ino,uint64_t end){assert(ino==42);if(end>logical_size)logical_size=end;}
static int accept_body(efs_ino_t ino,uint32_t ci,uint32_t n){
 assert(ino==42&&n);if(fail_second&&ci==1)return -1;
 bodies++;
 /* A reclaim flush finishes before the writer publishes its exact EOF.
  * The production snapshot skips pub_has_locked inodes. The old ordering
  * would carry a size-zero chunk and the server would infer 128 KiB. */
 pthread_mutex_lock(&g_client.dirty_mu);
 if(!pub_has_locked(ino))server_size=logical_size?logical_size:131072;
 pthread_mutex_unlock(&g_client.dirty_mu);
 assert(server_size==0);
 return 0;
}
static int dcache_store_full_chunk(efs_ino_t ino,uint32_t ci,const uint8_t *p,uint32_t n){(void)p;return accept_body(ino,ci,n);}
static int dcache_patch(efs_ino_t ino,uint32_t ci,uint32_t off,const uint8_t *p,uint32_t n){(void)off;(void)p;return accept_body(ino,ci,n);}
static int dcache_load_and_patch(efs_ino_t ino,uint32_t ci,uint32_t off,const uint8_t *p,uint32_t n,uint32_t cs){(void)ino;(void)ci;(void)off;(void)p;(void)n;(void)cs;return -1;}
'''+function('dcache_try_patch_run')+'\n'+function('efs_dcache_try_patch')+r'''
int main(void){
 uint8_t payload[512]={1};assert(!efs_dcache_try_patch(42,0,512,payload));
 assert(bodies==1&&logical_size==512&&server_size==512&&published==1);
 logical_size=server_size=0;
 int first=write_accept_begin(42),second=write_accept_begin(42);
 assert(first>=0&&second==first&&pub_slots[first].acceptors==2);
 write_accept_end(first);assert(pub_has_locked(42)&&pub_slots[first].acceptors==1);
 logical_size=512;write_accept_end(second);assert(!pub_has_locked(42)&&server_size==512);
 logical_size=server_size=0;fail_second=1;
 assert(efs_dcache_try_patch(42,131056,32,payload)==-1);
 assert(logical_size==131072&&server_size==131072&&!pub_has_locked(42));
 int previous=bodies;outer=report_clock_ms()-1;
 assert(efs_dcache_try_patch(42,0,512,payload)==-1&&bodies==previous&&!pub_has_locked(42));outer=0;
 int slots[PUB_SLOTS];pthread_mutex_lock(&g_client.dirty_mu);
 for(int i=0;i<PUB_SLOTS;i++){slots[i]=pub_take_locked(100+i,NULL,0);assert(slots[i]>=0);}
 pthread_mutex_unlock(&g_client.dirty_mu);outer=report_clock_ms()+5;
 assert(write_accept_begin(42)<0);outer=0;
 for(int i=0;i<PUB_SLOTS;i++)pub_slots[slots[i]]=(struct pub_slot){0};
 puts("write acceptance: early reclaim excludes REPORT until exact EOF, shared writers, partial failure and bounded admission PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-write-acceptance-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 cc=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))]
 subprocess.run(cc,check=True);subprocess.run([str(p.with_suffix(''))],check=True,timeout=15)
 legacy=code.replace('if (!pub_slots[i].used)','if (!pub_slots[i].used || pub_slots[i].acceptors)')
 assert legacy!=code;p.write_text(legacy);subprocess.run(cc,check=True)
 resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 rejected=subprocess.run([str(p.with_suffix(''))],capture_output=True,timeout=15)
 assert rejected.returncode!=0 and b'server_size' in rejected.stderr,'early REPORT must fail the exact EOF oracle'
 print('early size-zero REPORT negative control rejected PASS')
