#!/usr/bin/env python3
"""Actual outer PUT retry loop: no sleeps, reprobes or retries past ownership budget."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/write.c').read_text()
def fn(name):
 a=s.index(name+'(');a=s.rfind('\n',0,a)+1;return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/protocol.h"
#include "efs/put_ticket.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
static uint64_t deadline;static unsigned calls,admits,releases,invalidations,reprobes;static int terminal=EFS_ERR_NET;static int recover_busy,hard_fail,admit_rc;
static uint64_t report_clock_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
static void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
static int efs_client_rpc_past_deadline(void){return deadline&&report_clock_ms()>=deadline;}
static int efs_client_put_admit(efs_ino_t ino,uint32_t ci,const efs_node_id_t nodes[3],const uint8_t checksums[3][EFS_HASH_SIZE],uint32_t len,uint32_t off,uint32_t dlen,struct efs_put_ticket *t)
{(void)checksums;assert(ino==1&&!ci&&len==1&&!off&&!dlen&&nodes[2]==3&&deadline);admits++;if(admit_rc)return admit_rc;memset(t,0,sizeof(*t));t->ino=ino;t->object=77;t->id.seq=admits;memcpy(t->nodes,nodes,12);return EFS_OK;}
static void efs_client_put_release(uint64_t seq,int landed){assert(seq==admits&&!landed);releases++;}
/* K34: down_third marks member 3 down in this client; reprobe_ok says
 * whether it answers a probe. */
static int down_third,reprobe_ok;static unsigned probes;
static int efs_client_node_is_down(efs_node_id_t n){return down_third && n==3;}
static int efs_client_node_reprobe(efs_node_id_t n){assert(n==3);probes++;if(reprobe_ok)down_third=0;return reprobe_ok;}
static void reroute_untried_fragments(efs_ino_t i,uint32_t c,efs_node_id_t *n){(void)i;(void)c;(void)n;}
static void efs_client_conn_invalidate_node(efs_node_id_t n){(void)n;invalidations++;}
static void efs_client_nodes_force_reprobe(void){reprobes++;}
const char *efs_strerror(int rc){(void)rc;return "injected";}
/* W154: degrade=1 leaves fragment 2 unacknowledged on the OK attempt. */
static int degrade,note_rc=EFS_OK;static unsigned notes,logs;static uint32_t noted_mask;
/* K34: busy_third>0 makes member 3 answer BUSY on that many OK attempts. */
static int busy_third;
static int dgobj_note(efs_ino_t ino,uint32_t ci,uint64_t gen,uint32_t mask)
{(void)ino;(void)ci;assert(gen==77);notes++;noted_mask=mask;return note_rc;}
static void degraded_ack_log(efs_ino_t ino,uint32_t ci,uint64_t gen,const efs_node_id_t *nodes,int fi)
{(void)ino;(void)ci;(void)gen;(void)nodes;assert(fi==2);logs++;}
static int put_fragments_parallel_once(const struct efs_put_ticket *t,const uint8_t **fragments,int *failed,int *acked)
{(void)fragments;assert(t->object==77&&t->id.seq==admits);assert(deadline&&acked);calls++;for(unsigned i=0;i<3;i++)failed[i]=hard_fail?1:2;usleep(5000);
 int rc=recover_busy && calls>2 ? EFS_OK : terminal;
 /* Acks from an earlier attempt of the same object still count. */
 if(rc==EFS_OK)for(unsigned i=0;i<3;i++)if(!degrade||i!=2)acked[i]=1;
 if(rc==EFS_OK && busy_third){busy_third--;failed[2]=3;if(!busy_third)acked[2]=1;}
 return rc;}
'''+fn('report_retry_pause')+fn('put_fragments_parallel_run')+fn('efs_client_put_fragments_parallel')+r'''
#define PUT(t) efs_client_put_fragments_parallel(1,0,nodes,fragments,1,checksums,0,0,t)
int main(void){
 efs_node_id_t nodes[3]={1,2,3};const uint8_t *fragments[3]={0};const uint8_t checksums[3][EFS_HASH_SIZE]={{0}};struct efs_put_ticket t;
 uint64_t start=report_clock_ms();deadline=start;
 /* An expired budget admits nothing and sends nothing. */
 assert(PUT(&t)==EFS_ERR_BUSY);
 assert(!calls&&!admits&&!releases&&!t.object);
 start=report_clock_ms();deadline=start+15;
 assert(PUT(&t)==EFS_ERR_BUSY);
 assert(calls==1&&admits==1&&releases==1&&t.object==77&&!invalidations&&!reprobes&&report_clock_ms()-start<100);
 calls=0;terminal=EFS_ERR_QUOTA;deadline=report_clock_ms()+100;
 assert(PUT(&t)==EFS_ERR_QUOTA && calls==1 && releases==2);
 /* A refused ticket is terminal: never resent under the same ticket. */
 calls=0;terminal=EFS_ERR_STALE;deadline=report_clock_ms()+100;
 assert(PUT(&t)==EFS_ERR_STALE && calls==1 && releases==3);
 /* Success leaves the ticket to the caller's table update. */
 calls=0;terminal=EFS_OK;deadline=report_clock_ms()+100;
 assert(PUT(&t)==EFS_OK && calls==1 && releases==3);
 calls=0;terminal=EFS_ERR_BUSY;recover_busy=1;deadline=report_clock_ms()+500;unsigned before=admits;
 assert(PUT(&t)==EFS_OK);
 assert(calls==3&&admits==before+1&&!invalidations&&!reprobes);
 calls=0;recover_busy=0;deadline=report_clock_ms()+80;
 start=report_clock_ms();
 assert(PUT(&t)==EFS_ERR_BUSY);
 assert(calls<=2&&!invalidations&&!reprobes&&report_clock_ms()-start<150);
 calls=0;terminal=EFS_ERR_NET;hard_fail=1;deadline=report_clock_ms()+2000;before=admits;
 assert(PUT(&t)==EFS_ERR_NET);
 assert(calls==4&&admits==before+1&&invalidations==12&&!reprobes);
 hard_fail=0;invalidations=0;
 /* W154: a two-fragment success is recorded and logged; an unrecordable
  * one fails the write and gives its ticket up. */
 calls=0;terminal=EFS_OK;degrade=1;deadline=report_clock_ms()+100;
 assert(PUT(&t)==EFS_OK);
 assert(calls==1&&notes==1&&noted_mask==4&&logs==1);
 calls=0;note_rc=EFS_ERR_NOMEM;deadline=report_clock_ms()+100;before=releases;
 assert(PUT(&t)==EFS_ERR_NOMEM);
 assert(notes==2&&logs==1&&releases==before+1);
 degrade=0;note_rc=EFS_OK;
 calls=0;terminal=EFS_OK;deadline=report_clock_ms()+100;
 assert(PUT(&t)==EFS_OK&&notes==2);
 /* K34: two acks and a BUSY third member is not a degraded stripe: the
  * missing fragment is resent until stored, and a budget that runs out
  * fails the write rather than acknowledging two fragments. */
 calls=0;degrade=1;busy_third=3;deadline=report_clock_ms()+1000;
 assert(PUT(&t)==EFS_OK&&calls==3&&notes==2&&logs==1);
 calls=0;busy_third=1000;deadline=report_clock_ms()+120;before=releases;
 assert(PUT(&t)==EFS_ERR_BUSY&&notes==2&&logs==1&&releases==before+1&&calls<=4);
 busy_third=0;
 /* K34: a third member skipped only by a stale down-mark is probed once
  * before the stripe is acknowledged degraded naming it. */
 calls=0;down_third=1;reprobe_ok=1;deadline=report_clock_ms()+1000;unsigned n0=notes;
 assert(PUT(&t)==EFS_OK&&probes==1&&calls==2&&notes==n0+1);
 calls=0;down_third=1;reprobe_ok=0;probes=0;deadline=report_clock_ms()+1000;
 assert(PUT(&t)==EFS_OK&&probes==1&&calls==1&&notes==n0+2);
 down_third=0;degrade=0;
 /* An admission failure sends nothing. */
 calls=0;admit_rc=EFS_ERR_FENCED;deadline=report_clock_ms()+100;
 assert(PUT(&t)==EFS_ERR_FENCED && !calls);admit_rc=0;
 calls=0;terminal=EFS_ERR_QUOTA;deadline=0;
 assert(PUT(&t)==EFS_ERR_QUOTA && calls==1 && !deadline);
 return 0;
}
'''
# Moved from the retired tests/test_put_hint.py (W154): every outer attempt
# resends the same object (the admitted ticket) and unions its acknowledgements.
assert 'rc = put_fragments_parallel_once(ticket, fragments, failed, acked);' in s
with tempfile.TemporaryDirectory(prefix='efs-put-retry-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('PUT retry: admission inside the budget, one ticket per object across retries, inherited backoff, no expired sends/reprobes, terminal STALE/QUOTA/NET release the ticket, a BUSY third member completed or the write failed, never degraded, a down-marked third member probed once (K34) PASS')
