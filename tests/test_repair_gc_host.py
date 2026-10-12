#!/usr/bin/env python3
"""Actual host GC callback must neither delete nor acknowledge fenced objects."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text();a=s.index('static void host_gc_record(');fn=s[a:s.index('\n}',a)+2]
# Candidate integration exercised here, not installed in the default GC path.
fn=fn.replace('    for (fi = 0;', '    if(efs_repair_gc_object_ready(h->kv,ino,ci,gen)!=EFS_OK)return;\n    for (fi = 0;')
code=r'''
#include "efs/repair_job.h"
#include <assert.h>
#include <string.h>
#define GC_ACK_MAX 128
struct server {uint32_t id;};
struct efs_raft_host {struct server *s;void *kv;int gc_running;};
struct efs_export {uint32_t id;};
static int barrier,removed;
static uint64_t rd64be(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<8;i++)n=n*256+p[i];return n;}
static uint32_t rd32be(const uint8_t *p){return (uint32_t)rd64be((const uint8_t[8]){0,0,0,0,p[0],p[1],p[2],p[3]});}
static int env_on(const char *p){(void)p;return 0;}
int efs_meta_unpack_gc(const uint8_t *v,uint32_t n,uint32_t *nodes,uint8_t *ack,uint8_t sums[3][32]){(void)v;(void)n;*ack=0;nodes[0]=1;nodes[1]=2;nodes[2]=3;memset(sums,0,96);return 0;}
int efs_repair_gc_object_ready(struct efs_kv *kv,efs_ino_t ino,uint32_t ci,uint64_t obj){(void)kv;assert(ino==9&&ci==7&&obj==101);return barrier;}
static int host_gc_local_del(struct efs_raft_host *h,struct efs_export *ex,efs_ino_t ino,uint32_t ci,uint32_t fi,uint64_t gen,const uint8_t *sum){(void)h;(void)ex;(void)ino;(void)ci;(void)fi;(void)gen;(void)sum;removed++;return 0;}
static int host_gc_remote_del(struct efs_raft_host *h,uint32_t node,uint32_t ex,efs_ino_t ino,uint32_t ci,uint32_t fi,uint64_t gen,const uint8_t *sum){(void)h;(void)node;(void)ex;(void)ino;(void)ci;(void)fi;(void)gen;(void)sum;removed++;return 0;}
'''+fn+r'''
int main(void){struct server server={1};struct efs_raft_host h={.s=&server,.gc_running=1};struct efs_export ex={1};uint8_t key[24]={0},val[128]={0};key[10]=9;key[18]=101;key[23]=7;struct efs_gc_ack_item ack[128];int n=0;
 int failures[]={EFS_ERR_BUSY,EFS_ERR_PROTO,EFS_ERR_IO,EFS_ERR_NOMEM};
 for(unsigned i=0;i<4;i++){barrier=failures[i];host_gc_record(&h,&ex,key,val,ack,&n);assert(!removed&&!n);}
 barrier=0;host_gc_record(&h,&ex,key,val,ack,&n);assert(removed==3&&n==3);return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'gc.c';p.write_text('#include <stdio.h>\n'+code);b=Path(d)/'gc'
 subprocess.run(['cc','-std=gnu11','-I'+str(root/'include'),str(p),'-o',str(b)],check=True);subprocess.run([b],check=True)
print('actual GC callback no delete/ACK on pending/corrupt/IO journal PASS')
