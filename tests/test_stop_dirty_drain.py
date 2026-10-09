#!/usr/bin/env python3
"""Explicit stop drains unpublished partial bodies without reclaim's skip."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/write.c').read_text()
m=re.search(r'^int efs_dcache_drain_pending\(void\)\n\{',s,re.M)
function=s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
typedef uint64_t efs_ino_t;
#define DCACHE_SLOTS 4
#define EFS_OK 0
#define EFS_ERR_BUSY -1
#define EFS_ERR_IO -2
struct dcache_ent {efs_ino_t ino;int dirty;void *data;struct dcache_ent *next;};
static struct {struct dcache_ent e[DCACHE_SLOTS];} g_dcache;
static struct dcache_ent entries[75];static pthread_mutex_t mutex[DCACHE_SLOTS];
static unsigned calls,seen[100],expire_after;static efs_ino_t fail_ino;
static void dcache_ensure(void) {}
static pthread_mutex_t *dcache_mu(unsigned s) {return &mutex[s];}
static int efs_client_rpc_past_deadline(void) {return expire_after && calls>=expire_after;}
static int efs_dcache_flush_ino(efs_ino_t ino) {
 assert(ino<100);calls++;seen[ino]++;
 for(unsigned s=0;s<DCACHE_SLOTS;++s) {
  assert(!pthread_mutex_trylock(&mutex[s]));
  for(struct dcache_ent *e=&g_dcache.e[s];e;e=e->next)
   if(e->ino==ino && ino!=fail_ino)e->dirty=0;
  pthread_mutex_unlock(&mutex[s]);
 }
 return ino==fail_ino?EFS_ERR_IO:EFS_OK;
}
'''+function+r'''
static void setup(void) {
 calls=0;fail_ino=0;expire_after=0;
 for(unsigned i=0;i<100;++i)seen[i]=0;
 for(unsigned s=0;s<DCACHE_SLOTS;++s)g_dcache.e[s]=(struct dcache_ent){0};
 for(unsigned i=0;i<75;++i) {
  entries[i]=(struct dcache_ent){.ino=i+1,.dirty=1,.data=(void *)1};
  unsigned slot=i%DCACHE_SLOTS;
  entries[i].next=g_dcache.e[slot].next;g_dcache.e[slot].next=&entries[i];
 }
 /* Same inode in different slots must be flushed once per page. The
  * records represent new partial chunks; no published base is available. */
 entries[74].ino=1;entries[73].ino=1;
 entries[72].data=NULL;entries[71].dirty=0;
}
int main(void) {
 for(unsigned s=0;s<DCACHE_SLOTS;++s)assert(!pthread_mutex_init(&mutex[s],NULL));
 setup();assert(efs_dcache_drain_pending()==EFS_OK && calls==64);
 assert(efs_dcache_drain_pending()==EFS_OK && calls==71);
 for(unsigned i=1;i<=71;++i)assert(seen[i]==1);
 assert(efs_dcache_drain_pending()==EFS_OK && calls==71);
 setup();fail_ino=1;
 assert(efs_dcache_drain_pending()==EFS_ERR_IO && calls==64 && entries[0].dirty);
 assert(seen[2]+seen[3]+seen[4]>0); /* other files still progress */
 setup();expire_after=3;
 assert(efs_dcache_drain_pending()==EFS_ERR_BUSY && calls==3);
 for(unsigned s=0;s<DCACHE_SLOTS;++s)assert(!pthread_mutex_destroy(&mutex[s]));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-stop-dirty-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('stop dirty drain: partial bodies, bounded pages, duplicate inodes, unlocked I/O, failures and deadline PASS')

# Compile the real stop loop too: an empty pending queue is insufficient
# while a reporter still owns its detached in-flight page.
s=(root/'src/client/efs_fuse.c').read_text()
m=re.search(r'^static int efs_control_drain\([^;]+?\)\n\{',s,re.M)
control=s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#define EFS_OK 0
struct efs_report_pressure {uint64_t pending_records,inflight_records,reserved_records;};
static struct {int ready,count,inflight;pthread_mutex_t mu;} g_wb;
static uint64_t now,deadline;static unsigned rounds,flight_rounds;static int body_pending,lease_pending;
static uint64_t stats_now_ms(void) {return now;}
static uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
static void efs_client_rpc_set_deadline_ms(uint64_t value) {deadline=value;}
static void usleep(unsigned micros) {now+=(micros+999)/1000;}
static void efs_buf_drain_enter(void) {}
static void efs_buf_drain_leave(void) {}
static int efs_dcache_drain_pending(void) {rounds++;return EFS_OK;}
static int efs_client_report_dirty(int sync) {assert(sync);return EFS_OK;}
static int efs_open_drain_pending(void) {return lease_pending?-16:EFS_OK;}
static uint64_t efs_dcache_pending_records(const char *reason) {(void)reason;return body_pending;}
static void efs_client_report_pressure_stats(struct efs_report_pressure *p) {
 *p=(struct efs_report_pressure){.inflight_records=rounds<=flight_rounds};
}
'''+control+r'''
int main(void) {
 deadline=777;flight_rounds=3;
 assert(!efs_control_drain(100,0) && rounds==4 && deadline==777);
 rounds=0;now=0;flight_rounds=100;
 assert(efs_control_drain(25,0)==-1 && rounds==3 && deadline==777);
 rounds=0;now=0;flight_rounds=0;body_pending=1;
 assert(efs_control_drain(25,0)==-1 && rounds==3 && deadline==777);
 rounds=0;now=0;body_pending=0;lease_pending=1;
 assert(efs_control_drain(25,0)==-1 && rounds==3 && deadline==777);
 lease_pending=0;rounds=0;now=0;assert(!efs_control_drain(25,0) && rounds==1);
 rounds=0;assert(!efs_control_drain(25,1) && !rounds && deadline==777);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-stop-flight-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('stop drain: detached REPORT ownership, unresolved body refusal and deadline restoration PASS')
