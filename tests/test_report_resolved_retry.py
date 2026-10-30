#!/usr/bin/env python3
"""K38/K45: execute the publisher after a PARTIAL receipt, within one budget.

K38_WRITE_SOURCE selects the unchanged write.c for the negative control.
The adapter represents a detached run whose lost reply resolved PARTIAL;
production efs_client_report_dirty_ino must retain ownership and re-enter
publication, never turn that receipt directly into success or reset time.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('K38_WRITE_SOURCE', root / 'src/client/write.c')).read_text()
def extract(name):
    m = re.search(r'^(?:static )?int ' + name + r'\([^;]+?\)\n\{', source, re.M)
    assert m, name
    return source[m.start():source.index('\n}', m.end()) + 2]
function = ''
if 'static int report_dirty_ino_complete(' in source:
    function += extract('report_dirty_ino_complete') + '\n'
function += extract('efs_client_report_dirty_ino')
if 'int efs_client_report_pressure_ino(' in source:
    function += '\n' + extract('efs_client_report_pressure_ino')
code = r'''
#include "efs/common.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint64_t now, deadline, allowance=8000, first_deadline;
static int mode, calls, owned, published, flushes;
#define REPAIR_SCOPE_ARGS uint64_t end, int *resolved
static uint64_t report_clock_ms(void){return now;}
uint64_t efs_client_rpc_resolve_budget_ms(void){return allowance;}
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
int efs_client_rpc_past_deadline(void){return deadline && now>=deadline;}
void efs_client_put_settle_kick(void){}
static int unit_wait_idle(efs_ino_t i){(void)i;return EFS_OK;}
int efs_dcache_flush_ino(efs_ino_t i){(void)i;flushes++;return EFS_OK;}
int efs_dcache_flush_all(void){flushes++;return EFS_OK;}
static int report_dirty_ino_run(efs_ino_t ino,int sync, REPAIR_SCOPE_ARGS){
 *resolved=0;
 (void)ino;(void)sync;calls++;assert(owned);
 if(calls==1)first_deadline=deadline;
 else if(mode==3 && calls==2)assert(deadline==first_deadline); /* held unit keeps send budget */
 else assert(deadline==end); /* receipt recovery uses the one completion budget */
 if(mode==1){now+=100;return EFS_ERR_IO;} /* unknown receipt stays a failure */
 if(mode==2){now+=end>now && end-now<1000?end-now:1000;*resolved=1;return EFS_ERR_STALE;}
 if(mode==4){return EFS_ERR_STALE;}
 if(mode==3){if(calls==1)return EFS_ERR_AGAIN;}
 if(calls==1 || (mode==3 && calls==2)){
  /* The 8-second send budget expired; receipt resolved PARTIAL at 11 s. */
  now+=11000;*resolved=1;return EFS_ERR_STALE;
 }
 /* Only a real second publication proves completion of retained bytes. */
 assert(!deadline || now<deadline);published=1;owned=0;return EFS_OK;
}
''' + function + r'''
static void reset(int m){mode=m;now=1000;deadline=0;calls=published=flushes=0;owned=1;allowance=8000;}
int main(void){
 reset(0);assert(efs_client_report_dirty_ino(7,1)==EFS_OK);
 assert(calls==2 && published && !owned && !flushes && deadline==0 && now==12000);
 reset(1);assert(efs_client_report_dirty_ino(7,1)==EFS_ERR_IO && calls==1 && owned && !published);
 reset(2);assert(efs_client_report_dirty_ino(7,1)==EFS_ERR_STALE);
 assert(now==17000 && calls<=16 && owned && !published && deadline==0);
 reset(2);deadline=1500;assert(efs_client_report_dirty_ino(7,1)==EFS_ERR_STALE);
 assert(now==1500 && calls==1 && deadline==1500 && owned); /* inherited bound wins */
 reset(4);assert(efs_client_report_dirty_ino(7,1)==EFS_ERR_STALE && calls==1 && owned);
 reset(0);assert(efs_client_report_dirty_ino(7,0)==EFS_ERR_STALE && calls==1 && owned);
 reset(3);assert(efs_client_report_dirty_ino(7,1)==EFS_OK && calls==3 && flushes==1 && !owned);
 reset(2);allowance=0;assert(efs_client_report_dirty_ino(7,1)==EFS_ERR_STALE && now==9000 && owned);
 puts("K38/K45 receipt-STALE retry: owned bytes re-published; unknown fails; one absolute/inherited budget PASS");
 return 0;
}
'''
if 'int efs_client_report_pressure_ino(' in source:
    cases = r''' reset(3);assert(efs_client_report_pressure_ino(7)==EFS_OK && calls==1 && !flushes && owned && !published);
 reset(0);assert(efs_client_report_pressure_ino(7)==EFS_OK && calls==2 && !flushes && !owned && published);
 reset(1);assert(efs_client_report_pressure_ino(7)==EFS_ERR_IO && calls==1 && owned && !published);
'''
    code = code.replace(' puts("K38/K45 receipt-STALE retry:', cases + ' puts("K38/K45 receipt-STALE retry:')
if 'completion_deadline, &resolved_stale' not in function:
    code = code.replace('static int report_dirty_ino_run(efs_ino_t ino,int sync, REPAIR_SCOPE_ARGS){',
                        'static int report_dirty_ino_run(efs_ino_t ino,int sync){ uint64_t end=17000;int value,*resolved=&value;')
with tempfile.TemporaryDirectory(prefix='efs-resolved-retry-') as d:
    p = Path(d) / 'test.c'
    p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(root/'include'), str(p), '-o', str(Path(d)/'test')], check=True)
    subprocess.run([str(Path(d)/'test')], check=True)

# Execute the production receipt-resolution block itself. The former wrapper
# test mocked an entire run and could not detect freeing its sent snapshot
# before authoritative repull/replay. Old source must fail at runtime here.
a=source.index('    if (sync && report_sent && cn) {')
b=source.index('\n    if (dcache_trace_on())',a)
block=source[a:b]
helper=''
if 'static int report_replay_snapshot(' in source:
    helper=extract('report_replay_snapshot')
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
struct dirty_snap {int owned;};
struct efs_chunk_rec {uint64_t generation;};
struct efs_ino_size_rec {efs_ino_t ino;};
struct client {int export_id;} g_client;
static uint64_t deadline,now;
static int resolved_rc,replay_rc,replayed,inode_reads;
static struct dirty_snap expected_ds;
static struct efs_chunk_rec expected_rec[2]={{19},{23}};
static uint64_t expected_seq[2]={29,31};
static __attribute__((unused)) uint64_t report_mono_ms(void){return now;}
void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
int efs_client_rpc_past_deadline(void){return deadline && now>=deadline;}
static __attribute__((unused)) int report_replay_stale(efs_ino_t ino){assert(ino==7);inode_reads++;return 0;}
static __attribute__((unused)) int stale_repull_replay(struct dirty_snap *ds,efs_ino_t ino,
 const struct efs_chunk_rec *rec,const uint64_t *seq,uint32_t count){
 assert(ds==&expected_ds&&ds->owned&&ino==7&&rec==expected_rec&&seq==expected_seq&&count==2);
 assert(rec[0].generation==19&&rec[1].generation==23&&seq[0]==29&&seq[1]==31);
 assert(deadline==17000);replayed++;now+=50;return replay_rc;
}
static int efs_client_rpc_report_resolve(int export_id,const struct efs_chunk_rec *rec,uint32_t cn,
 const struct efs_ino_size_rec *ir,uint32_t in,int sync,const void *pub,uint64_t sent,int rc,efs_ino_t ino){
 (void)export_id;(void)ir;(void)in;(void)pub;assert(expected_ds.owned&&rec==expected_rec&&cn==2&&sync&&sent==1&&ino==7&&rc==EFS_ERR_NET);
 assert(deadline==17000);return resolved_rc;
}
'''+helper+r'''
static int run(int *resolved_stale){
 int sync=1,rc=EFS_ERR_NET;uint64_t report_sent=1,completion_deadline=17000,outer=9000,t_replay_ms=0;
 uint32_t cn=2,in=1;efs_ino_t only_ino=7;int pubid=0;
 struct dirty_snap ds=expected_ds;
 struct efs_chunk_rec *crecs=expected_rec;uint64_t *cseq=expected_seq;
 struct efs_ino_size_rec rows[1]={{7}},*irecs=rows;
 /* ds is a local snapshot in production: point the stub's expected address
  * at that local copy through a macro for the extracted block. */
 (void)ds;(void)cseq;(void)t_replay_ms;
'''+block+r'''
 assert(deadline==outer);
 return rc;
}
int main(void){
 expected_ds.owned=1;now=12000;deadline=9000;resolved_rc=EFS_ERR_STALE;
 int resolved=0;assert(run(&resolved)==EFS_ERR_STALE&&resolved&&replayed==1&&inode_reads==1);
 replayed=inode_reads=0;replay_rc=EFS_ERR_BUSY;resolved=1;
 assert(run(&resolved)==EFS_ERR_BUSY&&!resolved&&replayed==1&&expected_ds.owned);
 replayed=inode_reads=0;resolved_rc=EFS_ERR_IO;replay_rc=0;resolved=1;
 assert(run(&resolved)==EFS_ERR_IO&&!resolved&&!replayed&&!inode_reads&&expected_ds.owned);
 replayed=inode_reads=0;resolved_rc=EFS_ERR_STALE;now=17000;resolved=1;
 assert(run(&resolved)==EFS_ERR_BUSY&&!resolved&&!replayed&&!inode_reads&&expected_ds.owned);
 puts("late receipt PARTIAL: original sent snapshot replayed before release; unknown/replay failure retains ownership PASS");
}
'''
# The adapter verifies the local snapshot contents, not an invented new body.
code=code.replace('assert(ds==&expected_ds&&ds->owned', 'assert(ds->owned')
with tempfile.TemporaryDirectory(prefix='efs-late-replay-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror',
                   '-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
