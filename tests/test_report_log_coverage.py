#!/usr/bin/env python3
"""Production REPORT/background durability flags follow completed log coverage."""
from pathlib import Path
import subprocess
import re
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
def fn(name):
 # Require a definition: a forward declaration is not executable coverage.
 match=re.search(r'static int '+re.escape(name)+r'\([^;{]*\)\s*\{',s)
 assert match is not None,name
 a=match.start();return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
struct efs_raft_disk {int unused;};struct efs_raft {int unused;};
struct efs_raft_host {struct efs_raft_disk *disk;pthread_mutex_t mu;};
static uint64_t covered,synced=1000,last_marked;static unsigned marks,releases,kicks,broadcasts,applies;static int release_rc,durable_rc;
static uint64_t efs_raft_disk_synced_bytes(struct efs_raft_disk*d){assert(d);return synced;}
static uint64_t efs_raft_disk_covered_index(struct efs_raft_disk*d,uint32_t g,uint64_t end){assert(d && g==2 && end==synced);return covered;}
static int efs_raft_durable(struct efs_raft*r,uint64_t i){assert(r);marks++;applies++;last_marked=i;return durable_rc;}
static int efs_raft_note_durable(struct efs_raft*r,uint64_t i){assert(r);marks++;last_marked=i;return durable_rc;}

static int efs_raft_disk_sync_release_wait(struct efs_raft_disk*d){assert(d);releases++;return release_rc;}
static void host_note_durable_synced(struct efs_raft_host*h){assert(h);broadcasts++;}
static void host_pump_kick(struct efs_raft_host*h){assert(h);kicks++;}
''' +fn('host_mark_durable_covered')+fn('host_report_release')+r'''
int main(void){struct efs_raft_disk disk={0};struct efs_raft raft={0};struct efs_raft_host h={.disk=&disk,.mu=PTHREAD_MUTEX_INITIALIZER};
 covered=40;assert(!host_mark_durable_covered(&h,2,&raft,41) && !marks);
 covered=41;assert(!host_mark_durable_covered(&h,2,&raft,41) && marks==1 && last_marked==41);
 covered=45;durable_rc=EFS_ERR_IO;assert(host_mark_durable_covered(&h,2,&raft,41)==EFS_ERR_IO && last_marked==45);
 release_rc=EFS_ERR_IO;assert(host_report_release(&h,1)==EFS_ERR_IO && releases==1 && !broadcasts && !kicks);
 release_rc=EFS_OK;assert(!host_report_release(&h,1) && releases==2 && broadcasts==1 && kicks==1);
 assert(!host_report_release(&h,0) && releases==2 && broadcasts==2 && kicks==2);
 assert(!applies); /* Disk-backed handler paths only note; no apply callback. */
 h.disk=NULL;durable_rc=0;assert(!host_mark_durable_covered(&h,2,&raft,99) && applies==1 && last_marked==99);
 pthread_mutex_destroy(&h.mu);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-log-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
# Check all three former unchecked index announcements use the tested guard.
for name,end in [('host_propose_impl','static int host_propose('),('host_bg_propose','static int host_gc_propose(')]:
 a=s.index('static int '+name+'(');b=s.index(end,a)
 assert 'rc = efs_raft_durable(r,' not in s[a:b]
a=s.index('void server_raft_host_report(');b=s.index('\n}',a)
assert 'efs_raft_durable(' not in s[a:b]
assert 'host_report_release(h, held)' in s[a:b]
print('REPORT/shared log: uncovered suffix waits, covered index only, sync failure refusal and background call sites PASS')
