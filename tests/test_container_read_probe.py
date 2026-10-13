#!/usr/bin/env python3
"""Actual bounded container sender probe; original replication stays owed."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('static void host_container_read_probe(');fn=s[a:s.index('\n}',a)+2]
timing = 'efs_batch_obs_now_us' in fn
code=r'''
#include "efs/raft_flow.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define HOST_NGROUPS 2
#define HOST_OUTBOX_MAX 4
#define HOST_OUTBOX_BYTES 512
#define EFS_WIRE_RAFT_HDR_LEN 80
static int observer_on;
#define BO_ON() observer_on
#define efs_batch_obs_now_us() UINT64_C(42)
struct host_outbox_item {uint8_t *buf;uint32_t len; TIMING_FIELD };
struct host_outbox {int preserve_read,npri,nent;uint64_t queued_bytes,queued_bytes_hi,st_enq;uint32_t st_hi;uint64_t read_probe_context[2],read_probe_term[2],read_probe_boot[2];struct host_outbox_item pri[4];};
static int fail_alloc;
static void *probe_alloc(size_t n){return fail_alloc?NULL:malloc(n);}
#define malloc probe_alloc
'''+fn+r'''
#undef malloc
int main(void){
 struct host_outbox tx={0};uint8_t p[100],original[100];memset(p,0,sizeof(p));
 p[0]=EFS_RAFT_MSG_AE_REQ;p[1]=EFS_RAFT_GROUP_SHARD;p[19]=3;p[27]=5;p[35]=7;p[43]=8;p[51]=9;p[59]=10;p[67]=11;p[75]=12;p[79]=1;p[80]=99;memcpy(original,p,100);
 host_container_read_probe(&tx,p,100);assert(!tx.npri);tx.preserve_read=1;
 fail_alloc=1;host_container_read_probe(&tx,p,100);assert(!tx.npri&&!tx.read_probe_context[0]);fail_alloc=0;
 tx.nent=1;tx.queued_bytes=100;host_container_read_probe(&tx,p,100);
 assert(tx.nent==1&&tx.npri==1&&tx.queued_bytes==180&&tx.queued_bytes_hi==180&&tx.st_hi==2&&tx.st_enq==1);
 assert(!memcmp(p,original,100));uint8_t *q=tx.pri[0].buf; TIMING_OFF
 assert(tx.pri[0].len==80&&q[2]==1&&!q[3]&&q[35]==7&&q[19]==3&&q[27]==5&&q[67]==11);
 for(int i=36;i<60;i++)assert(!q[i]);for(int i=68;i<80;i++)assert(!q[i]);
 host_container_read_probe(&tx,p,100);assert(tx.npri==1);p[35]=6;host_container_read_probe(&tx,p,100);assert(tx.npri==1);
 p[35]=8;tx.queued_bytes=500;host_container_read_probe(&tx,p,100);assert(tx.npri==1&&tx.read_probe_context[0]==7);tx.queued_bytes=180;
 tx.nent=3;host_container_read_probe(&tx,p,100);assert(tx.npri==1);tx.nent=1;
 observer_on=1;host_container_read_probe(&tx,p,100);assert(tx.npri==2&&tx.read_probe_context[0]==8); TIMING_ON observer_on=0;
 p[1]=EFS_RAFT_GROUP_SHARD2;p[35]=1;host_container_read_probe(&tx,p,100);assert(tx.npri==3&&tx.read_probe_context[1]==1);
 for(int i=0;i<tx.npri;i++)free(tx.pri[i].buf);tx.npri=0;tx.nent=0;tx.queued_bytes=0;
 p[1]=EFS_RAFT_GROUP_SHARD;p[27]=6;p[35]=1;host_container_read_probe(&tx,p,80);assert(tx.npri==1);free(tx.pri[0].buf);tx.npri=0;tx.queued_bytes=0;
 p[19]=4;host_container_read_probe(&tx,p,80);assert(tx.npri==1);free(tx.pri[0].buf);tx.npri=0;tx.queued_bytes=0;
 p[35]=0;host_container_read_probe(&tx,p,80);assert(!tx.npri);p[35]=2;p[0]=EFS_RAFT_MSG_AE_REP;host_container_read_probe(&tx,p,80);assert(!tx.npri);p[0]=EFS_RAFT_MSG_AE_REQ;p[1]=99;host_container_read_probe(&tx,p,80);assert(!tx.npri);p[1]=0;host_container_read_probe(&tx,p,79);assert(!tx.npri);
 puts("container read probe: bounded priority vote, original bytes/progress retained, scoped/dedup/failure guards PASS");return 0;
}
'''
code=code.replace('TIMING_FIELD','uint64_t queued_us;' if timing else '').replace('TIMING_OFF','assert(!tx.pri[0].queued_us);' if timing else '').replace('TIMING_ON','assert(tx.pri[1].queued_us==42);' if timing else '')
with tempfile.TemporaryDirectory(prefix='efs-container-probe-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',*shlex.split(os.environ.get('EFS_TEST_CFLAGS','')),'-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
