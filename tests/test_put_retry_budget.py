#!/usr/bin/env python3
"""Actual outer PUT retry loop: no sleeps, reprobes or retries past ownership budget."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/write.c').read_text()
def fn(name):
 a=s.index(name+'(');a=s.rfind('\n',0,a)+1;return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
static uint64_t deadline;static unsigned calls,gens,invalidations,reprobes;static int terminal=EFS_ERR_NET;static int recover_busy,hard_fail;
static uint64_t report_clock_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
static void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
static int efs_client_rpc_past_deadline(void){return deadline&&report_clock_ms()>=deadline;}
static uint64_t new_chunk_object_gen(uint32_t c){(void)c;gens++;return 77;}
static int efs_client_node_is_down(efs_node_id_t n){(void)n;return 0;}
static void reroute_untried_fragments(efs_node_id_t *n){(void)n;}
static void efs_client_conn_invalidate_node(efs_node_id_t n){(void)n;invalidations++;}
static void efs_client_nodes_force_reprobe(void){reprobes++;}
const char *efs_strerror(int rc){(void)rc;return "injected";}
static int put_fragments_parallel_once(efs_ino_t ino,uint32_t ci,const efs_node_id_t *nodes,const uint8_t **fragments,uint32_t len,const uint8_t checksums[3][EFS_HASH_SIZE],int *failed,uint64_t gen,int first)
{(void)ino;(void)ci;(void)nodes;(void)fragments;(void)len;(void)checksums;assert(gen==77&&first==(calls==0));assert(deadline);calls++;for(unsigned i=0;i<3;i++)failed[i]=hard_fail?1:2;usleep(5000);return recover_busy && calls>2 ? EFS_OK : terminal;}
'''+fn('report_retry_pause')+fn('put_fragments_parallel_run')+fn('efs_client_put_fragments_parallel')+r'''
int main(void){
 efs_node_id_t nodes[3]={1,2,3};const uint8_t *fragments[3]={0};const uint8_t checksums[3][EFS_HASH_SIZE]={{0}};uint64_t object=99;
 uint64_t start=report_clock_ms();deadline=start;
 assert(efs_client_put_fragments_parallel(1,0,nodes,fragments,1,checksums,&object)==EFS_ERR_BUSY);
 assert(!calls&&!gens&&object==99);
 start=report_clock_ms();deadline=start+15;
 assert(efs_client_put_fragments_parallel(1,0,nodes,fragments,1,checksums,&object)==EFS_ERR_BUSY);
 assert(calls==1&&gens==1&&object==77&&!invalidations&&!reprobes&&report_clock_ms()-start<100);
 calls=gens=0;terminal=EFS_ERR_QUOTA;deadline=report_clock_ms()+100;
 assert(efs_client_put_fragments_parallel(1,0,nodes,fragments,1,checksums,&object)==EFS_ERR_QUOTA && calls==1);
 calls=gens=0;terminal=EFS_OK;deadline=report_clock_ms()+100;
 assert(efs_client_put_fragments_parallel(1,0,nodes,fragments,1,checksums,&object)==EFS_OK && calls==1);
 calls=gens=0;terminal=EFS_ERR_BUSY;recover_busy=1;deadline=report_clock_ms()+500;
 assert(efs_client_put_fragments_parallel(1,0,nodes,fragments,1,checksums,&object)==EFS_OK);
 assert(calls==3&&gens==1&&!invalidations&&!reprobes);
 calls=gens=0;recover_busy=0;deadline=report_clock_ms()+80;
 start=report_clock_ms();
 assert(efs_client_put_fragments_parallel(1,0,nodes,fragments,1,checksums,&object)==EFS_ERR_BUSY);
 assert(calls<=2&&gens==1&&!invalidations&&!reprobes&&report_clock_ms()-start<150);
 calls=gens=0;terminal=EFS_ERR_NET;hard_fail=1;deadline=report_clock_ms()+2000;
 assert(efs_client_put_fragments_parallel(1,0,nodes,fragments,1,checksums,&object)==EFS_ERR_NET);
 assert(calls==4&&gens==1&&invalidations==12&&!reprobes);
 hard_fail=0;invalidations=0;
 calls=gens=0;terminal=EFS_ERR_QUOTA;deadline=0;
 assert(efs_client_put_fragments_parallel(1,0,nodes,fragments,1,checksums,&object)==EFS_ERR_QUOTA && calls==1 && !deadline);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-put-retry-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('PUT retry: inherited backoff, no expired sends/reprobes, stable object and terminal outcomes PASS')
