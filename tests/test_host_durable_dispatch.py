#!/usr/bin/env python3
"""Handler durability notes cannot apply outside the pump's KV hold."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('static void host_durable_synced_mode(');b=s.index('/* h->mu held. An outer REPORT',a)
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define HOST_NGROUPS 2
#define EFS_RAFT_LEADER 2
struct efs_raft {int role;uint64_t noted,applied;};
struct efs_raft_disk {uint64_t synced,covered[3];};
struct host_group {int hosted;struct efs_raft*r;uint8_t group;};
struct efs_raft_host {struct efs_raft_disk*disk;struct host_group g[2];};
static int efs_raft_role(struct efs_raft*r){return r->role;}
static uint64_t efs_raft_disk_synced_bytes(struct efs_raft_disk*d){return d->synced;}
static uint64_t efs_raft_disk_covered_index(struct efs_raft_disk*d,uint8_t group,uint64_t synced){assert(synced==d->synced);return d->covered[group];}
static int efs_raft_note_durable(struct efs_raft*r,uint64_t index){if(index>r->noted)r->noted=index;return 0;}
static int efs_raft_durable(struct efs_raft*r,uint64_t index){efs_raft_note_durable(r,index);r->applied=r->noted;return 0;}
'''+s[a:b]+r'''
int main(void){struct efs_raft r[2]={{.role=2},{.role=2}};
 struct efs_raft_disk d={.synced=100,.covered={10,0,20}};
 struct efs_raft_host h={.disk=&d,.g={{1,&r[0],0},{1,&r[1],2}}};
 host_note_durable_synced(&h);assert(r[0].noted==10 && r[1].noted==20 && !r[0].applied && !r[1].applied);
 d.covered[0]=8;host_note_durable_synced(&h);assert(r[0].noted==10);
 host_durable_synced(&h);assert(r[0].applied==10 && r[1].applied==20);
 r[0].role=0;d.covered[0]=30;h.g[1].hosted=0;
 host_note_durable_synced(&h);assert(r[0].noted==10 && r[1].noted==20);
 h.disk=NULL;host_note_durable_synced(&h);assert(r[0].noted==10);
 puts("host durability: only synced hosted leader prefixes noted; handler never applies, pump applies under KV hold PASS");
}
'''
for name in ('host_propose_impl','host_report_release'):
 a=s.index('static int '+name+'(')
 if name=='host_report_release':a=s.index('static int host_report_release(struct efs_raft_host *h, int held)\n{')
 b=s.index('\n}',a);assert 'host_durable_synced(h)' not in s[a:b]
 assert 'host_note_durable_synced(h)' in s[a:b]
a=s.index('static void *host_pump(');b=s.index('\n}',a);body=s[a:b]
assert body.index('efs_kv_lsm_sync_hold(h->kv)')<body.index('host_durable_synced(h)')<body.index('efs_kv_lsm_sync_release(h->kv)')
with tempfile.TemporaryDirectory(prefix='efs-host-durable-dispatch-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
