#!/usr/bin/env python3
"""Production serial and fan-out pulls must not wrap the final chunk group."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/ops.c').read_text()
def fn(name):
 m=re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{',s,re.M);return s[m.start():s.index('\n}',m.end())+2]
a=s.index('struct pull_fan {');fan=s[a:s.index('\n};',a)+3]
code=r'''
#include "client_internal.h"
#include <assert.h>
#include <stdint.h>
struct efs_client g_client;
struct pull_absent {uint8_t *bits;uint32_t base,end;};
#define PULL_FAN 16
static unsigned calls;static uint32_t lower,upper;
static __thread uint64_t deadline;
uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t d) {deadline=d;}
int efs_client_rpc_past_deadline(void) {return 0;}
static void pull_absent_mark(struct pull_absent *ab,uint32_t lo,uint32_t hi) {(void)ab;assert(lo>=lower&&hi<=upper&&lo<hi);}
static void apply_chunk_recs(efs_ino_t ino,const struct efs_chunk_rec *r,uint32_t n) {(void)ino;(void)r;(void)n;}
static int fake_view(const struct efs_chunk_rec *r) {(void)r;return EFS_OK;}
#define efs_chunk_rec_view_valid fake_view
int efs_client_rpc_getchunks(efs_export_id_t ex,efs_ino_t ino,uint32_t start,struct efs_chunk_rec *r,uint32_t *n) {(void)ex;(void)ino;(void)r;assert(start>=lower&&start<upper&&*n>0&&*n<=64);__sync_fetch_and_add(&calls,1);*n=0;return EFS_OK;}
'''+fn('pull_group_end')+fn('pull_chunks_range')+fan+fn('pull_fan_thread')+fn('pull_groups_parallel')+r'''
int main(void) {
 lower=UINT32_MAX-130;upper=UINT32_MAX;
 assert(pull_group_end(UINT32_MAX-1,UINT32_MAX)==UINT32_MAX);
 assert(!pull_chunks_range(1,lower,upper,NULL)&&calls==3);
 calls=0;assert(!pull_groups_parallel(1,lower,upper,NULL)&&calls==3);
 lower=0;upper=65;calls=0;assert(!pull_chunks_range(1,lower,upper,NULL)&&calls==2);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pull-boundary-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('GETCHUNKS grouping: final uint32 group, serial/fanout and partial tail PASS')
