#!/usr/bin/env python3
"""Files and containers share admission-before-probe ticket correctness."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('static uint64_t host_read_register(');b=s.index('static int host_read_index_admitted(',a)
code=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
struct efs_raft {uint64_t round,term;};
struct host_group {uint64_t read_admit_count,read_begin_admits,read_begin_round,read_begin_term;};
struct server {int storage_engine;};
struct efs_raft_host {struct server *s;struct host_group g;};
static struct host_group *group_slot(struct efs_raft_host*h,uint8_t g){assert(g==0);return &h->g;}
static uint64_t efs_raft_read_round(struct efs_raft*r){return r->round;}
static uint64_t efs_raft_term(struct efs_raft*r){return r->term;}
static int efs_raft_read_begin(struct efs_raft*r){r->round++;return 0;}
'''+s[a:b]+r'''
int main(void){for(int engine=0;engine<=1;engine++){
 struct server server={engine};struct efs_raft_host h={.s=&server};struct efs_raft r={7,11};
 uint64_t a=host_read_register(&h,0),b=host_read_register(&h,0);assert(a==1&&b==2);
 assert(!host_read_begin_tracked(&h,0,&r)&&r.round==8);
 assert(host_read_eligible_round(&h,0,&r,a)==8&&host_read_eligible_round(&h,0,&r,b)==8);
 uint64_t late=host_read_register(&h,0);assert(late==3&&host_read_eligible_round(&h,0,&r,late)==9);
 r.term++;assert(host_read_eligible_round(&h,0,&r,a)==9);
 h.g.read_admit_count=UINT64_MAX-1;assert(!host_read_register(&h,0));
 assert(host_read_eligible_round(&h,0,&r,0)==9);
 }return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-tickets-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('Read admission: files/containers pre-probe cohort, late admission requires next round, term and saturation refusal PASS')
