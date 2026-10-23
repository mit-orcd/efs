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
static int efs_client_node_is_down(efs_node_id_t n){(void)n;return 0;}
static void reroute_untried_fragments(efs_node_id_t *n){(void)n;}
static void efs_client_conn_invalidate_node(efs_node_id_t n){(void)n;invalidations++;}
static void efs_client_nodes_force_reprobe(void){reprobes++;}
const char *efs_strerror(int rc){(void)rc;return "injected";}
static int put_fragments_parallel_once(const struct efs_put_ticket *t,const uint8_t **fragments,int *failed)
{(void)fragments;assert(t->object==77&&t->id.seq==admits);assert(deadline);calls++;for(unsigned i=0;i<3;i++)failed[i]=hard_fail?1:2;usleep(5000);return recover_busy && calls>2 ? EFS_OK : terminal;}
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
 /* An admission failure sends nothing. */
 calls=0;admit_rc=EFS_ERR_FENCED;deadline=report_clock_ms()+100;
 assert(PUT(&t)==EFS_ERR_FENCED && !calls);admit_rc=0;
 calls=0;terminal=EFS_ERR_QUOTA;deadline=0;
 assert(PUT(&t)==EFS_ERR_QUOTA && calls==1 && !deadline);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-put-retry-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('PUT retry: admission inside the budget, one ticket per object across retries, inherited backoff, no expired sends/reprobes, terminal STALE/QUOTA/NET release the ticket PASS')
