#!/usr/bin/env python3
"""Reproduce shared receipt-window admission while the old owner needs our lock.

Uses production admission and lease begin/end with a real contended mutex.
EFS_OPID_SOURCE selects old production code for an executing negative control.
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = Path(os.getenv('EFS_OPID_SOURCE',root/'src/client/inode_rpc.c')).read_text()


def fn(name):
    m = re.search(r'^(?:static )?[^\n;]*\b'+name+r'\([^;]+?\)\n\{', s, re.M)
    return s[m.start():s.index('\n}',m.end())+2] if m else ''


code = r'''
#include "client_internal.h"
#include "efs/session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
struct efs_client g_client;
static pthread_mutex_t opid_mu=PTHREAD_MUTEX_INITIALIZER,edge=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t opid_cv=PTHREAD_COND_INITIALIZER;
static _Thread_local uint64_t deadline;
static uint64_t now_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
int efs_client_rpc_past_deadline(void) {return deadline && now_ms()>=deadline;}
static void opid_seed_locked(void) {
 if(!g_client.opid_next) {g_client.opid_next=1;g_client.opid_epoch=1;g_client.opid_uuid[0]=1;}
}
''' + '\n'.join(fn(n) for n in ('opid_take_locked','opid_begin','opid_try_begin',
                                'opid_end','efs_client_lease_begin')) + r'''
static void *old_owner(void *unused) {
 (void)unused;
 pthread_mutex_lock(&edge);opid_end(0);pthread_mutex_unlock(&edge);
 return NULL;
}
int main(void) {
 struct efs_opid_req q;
 for(unsigned i=0;i<EFS_OPID_REPLY_CACHE;i++)assert(opid_begin(&q)==(int)i);
 for(unsigned i=1;i<EFS_OPID_REPLY_CACHE;i++)opid_end(i);
 uint64_t next=g_client.opid_next;
 pthread_mutex_lock(&edge);
 pthread_t old;assert(!pthread_create(&old,NULL,old_owner,NULL));
 struct efs_client_lease_request r={0};
 uint64_t begin=now_ms();deadline=begin+500;
 int rc=efs_client_lease_begin(&r,4097,99,EFS_LEASE_EDGE_OPEN,0);
 uint64_t elapsed=now_ms()-begin;
 /* Retire the old owner before any assertion can fail: no stuck thread or
  * abandoned mutation is required to demonstrate the old deadline cycle. */
 pthread_mutex_unlock(&edge);assert(!pthread_join(old,NULL));
 assert(rc==EFS_ERR_BUSY && !r.active && !r.q.id.seq && g_client.opid_next==next);
 assert(elapsed<200);
 deadline=now_ms()+500;pthread_mutex_lock(&edge);
 assert(!efs_client_lease_begin(&r,4097,99,EFS_LEASE_EDGE_OPEN,0));
 assert(r.active && r.q.id.seq==next && r.q.ack==next-1);
 opid_end(r.slot);pthread_mutex_unlock(&edge);
 memset(&r,0,sizeof(r));next=g_client.opid_next;deadline=now_ms()-1;
 assert(efs_client_lease_begin(&r,4097,99,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_BUSY);
 assert(!r.active && g_client.opid_next==next);
 puts("lease admission: no identity/wait under a saturated owner lock, old owner progresses, exact next ACK/identity and expired admission refused PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-lease-admit-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(shlex.split(os.getenv('CC','cc'))+['-std=gnu11','-D_GNU_SOURCE',
        '-Wall','-Wextra','-Werror','-pthread','-g','-O1','-I'+str(root/'include'),
        '-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
