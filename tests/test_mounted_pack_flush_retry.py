#!/usr/bin/env python3
"""Actual flush loop: transient authority refusal retains one immutable owner."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source=Path(os.environ.get('EFS_PACK_WRITE_SOURCE',root/'src/client/pack_write.c')).read_text()
a=source.index('static int pack_flush_run(');b=source.index('\nstatic int pack_drain_run(',a)
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
struct pack_context {struct {unsigned count;struct {efs_ino_t ino;} records[2];} cohort;
 struct {int done;} writers[2];uint64_t object;unsigned put_mask;};
static pthread_mutex_t pack_mu=PTHREAD_MUTEX_INITIALIZER;
static struct pack_context *pack_groups[2];
static struct pack_context owner;
static uint64_t deadline,now;
static int status,calls,pauses,expired,succeeds;
static int efs_client_pack_enabled(void){return 1;}
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t v){deadline=v;}
static uint64_t pack_now(void){return now*1000000;}
static int pack_pause(void){pauses++;now++;return expired?EFS_ERR_BUSY:EFS_OK;}
static int pack_flush_group(unsigned gi,int force){
 assert(gi==0&&!force&&pack_groups[gi]==&owner);
 assert(owner.object==77&&owner.put_mask==5&&owner.cohort.records[0].ino==123);
 calls++;if(succeeds&&calls==3){owner.writers[0].done=1;return EFS_OK;}return status;
}
'''+source[a:b]+r'''
static void reset(int rc){owner=(struct pack_context){.object=77,.put_mask=5};
 owner.cohort.count=1;owner.cohort.records[0].ino=123;pack_groups[0]=&owner;
 status=rc;calls=pauses=expired=succeeds=0;now=1;deadline=99;}
int main(void){
 reset(EFS_ERR_NOT_PRIMARY);succeeds=1;
 assert(!efs_client_pack_flush(123)&&calls==3&&pauses==3&&deadline==99);
 reset(EFS_ERR_BUSY);succeeds=1;
 assert(!efs_client_pack_flush(123)&&calls==3&&deadline==99);
 int hard[]={EFS_ERR_IO,EFS_ERR_PROTO,EFS_ERR_STALE,EFS_ERR_INVAL,EFS_ERR_NET};
 for(unsigned i=0;i<sizeof(hard)/sizeof(hard[0]);i++){
  reset(hard[i]);assert(efs_client_pack_flush(123)==hard[i]&&calls==1&&!pauses&&deadline==99);
 }
 reset(EFS_ERR_NOT_PRIMARY);expired=1;
 assert(efs_client_pack_flush(123)==EFS_ERR_BUSY&&calls==1&&pauses==1&&deadline==99);
 reset(EFS_ERR_NOT_PRIMARY);expired=1;deadline=0;
 assert(efs_client_pack_flush(123)==EFS_ERR_BUSY&&!deadline);
 reset(EFS_ERR_NOT_PRIMARY);assert(!efs_client_pack_flush(999)&&!calls&&!pauses);
 puts("mounted packed flush: immutable authority retry, retained ACK bits, deadline restoration and terminal failures PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-flush-retry-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-pthread','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
