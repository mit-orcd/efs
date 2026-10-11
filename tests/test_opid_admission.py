#!/usr/bin/env python3
"""Saturate real namespace mutation admission; no unidentified RPC may escape."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
code=(root/'src/client/inode_rpc.c').read_text()
def function(name):
    match=re.search(r'^(?:static )?[^\n;]*\b'+name+r'\([^;]+?\)\n\{',code,re.M)
    assert match,name
    return code[match.start():code.index('\n}',match.end())+2]
source=r'''
#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/opid.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
struct efs_client g_client;
static pthread_mutex_t opid_mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t test_deadline;
static uint64_t mono_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
int efs_client_rpc_past_deadline(void) {return test_deadline && mono_ms()>=test_deadline;}
uint64_t efs_client_rpc_deadline_ms(void) {return test_deadline;}
static unsigned calls;
static uint64_t last_ack,last_seq;
static int rpc_status_to_efs(uint8_t status) { return status?EFS_ERR_IO:EFS_OK; }
static int send(uint8_t type,const void *buf,uint32_t length,void *out,uint32_t outlen) {
    (void)type;assert(length>EFS_OPID_WIRE_LEN);calls++;
    struct efs_opid_req q;
    efs_opid_req_unpack(&q,(const uint8_t *)buf+length-EFS_OPID_WIRE_LEN);
    assert(efs_opid_req_valid(&q));last_ack=q.ack;last_seq=q.id.seq;
    assert(outlen==sizeof(struct efs_msg_inode_reply));memset(out,0,outlen);
    struct efs_msg_inode_reply *reply=out;reply->inode.parent=1;
    strcpy(reply->inode.name,"test");return EFS_OK;
}
static int rpc_send_recv_owner(efs_ino_t ino,uint8_t type,const void *buf,uint32_t length,
                              uint8_t reply,void *out,uint32_t outlen) {
    (void)ino;(void)reply;return send(type,buf,length,out,outlen);
}
static int rpc_send_recv_dual(uint8_t type,const void *buf,uint32_t length,uint8_t reply,
                              void *out,uint32_t outlen,int unused) {
    (void)reply;(void)unused;return send(type,buf,length,out,outlen);
}
'''+'\n'.join(function(name) for name in ('opid_seed_locked','opid_begin','opid_end','opid_suffix',
                                        'efs_client_rpc_create','efs_client_rpc_unlink',
                                        'efs_client_rpc_rename_at','efs_client_rpc_link'))+r'''
static pthread_mutex_t hold_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cv=PTHREAD_COND_INITIALIZER;
static unsigned ready;
static int release_workers,slots[EFS_OPID_REPLY_CACHE];
static struct efs_opid_req concurrent[EFS_OPID_REPLY_CACHE];
static void *hold_identity(void *value) {
    unsigned i=(unsigned)(uintptr_t)value;
    slots[i]=opid_begin(&concurrent[i]);assert(slots[i]>=0);
    pthread_mutex_lock(&hold_mu);ready++;pthread_cond_broadcast(&hold_cv);
    while(!release_workers)pthread_cond_wait(&hold_cv,&hold_mu);
    pthread_mutex_unlock(&hold_mu);opid_end(slots[i]);return NULL;
}
static void *release_oldest(void *value) {
    struct timespec delay={1,250000000};nanosleep(&delay,NULL);
    opid_end((int)(intptr_t)value);return NULL;
}
int main(void) {
    struct efs_opid_req held[EFS_OPID_REPLY_CACHE],q;
    for(int i=0;i<EFS_OPID_REPLY_CACHE;i++)assert(opid_begin(&held[i])==i);
    uint64_t next=g_client.opid_next;
    assert(efs_client_rpc_create(1,1,"test",S_IFREG|0600,0,0,0,NULL,NULL)==EFS_ERR_BUSY);
    assert(efs_client_rpc_unlink(1,1,"test",0)==EFS_ERR_BUSY);
    assert(efs_client_rpc_rename_at(1,1,"test",1,"new",NULL)==EFS_ERR_BUSY);
    assert(efs_client_rpc_link(1,2,1,"test",NULL)==EFS_ERR_BUSY);
    assert(!calls && g_client.opid_next==next);
    uint64_t begin=mono_ms();test_deadline=begin+20;
    assert(opid_begin(&q)<0 && !q.id.seq && g_client.opid_next==next);
    assert(mono_ms()-begin<200);test_deadline=0;
    opid_end(7);
    /* A free object slot is insufficient: higher known sequences are still
     * unacknowledgeable while the oldest remains unresolved. */
    assert(efs_client_rpc_unlink(1,1,"test",0)==EFS_ERR_BUSY && !calls);
    opid_end(0);
    assert(efs_client_rpc_unlink(1,1,"test",0)==EFS_OK);
    assert(calls==1 && last_seq==next && last_ack==held[1].id.seq-1);
    assert(g_client.opid_inflight[1]==held[1].id.seq);
    for(int i=0;i<EFS_OPID_REPLY_CACHE;i++)opid_end(i);
    pthread_t workers[EFS_OPID_REPLY_CACHE];pthread_attr_t attr;
    assert(!pthread_attr_init(&attr));assert(!pthread_attr_setstacksize(&attr,256*1024));
    for(unsigned i=0;i<EFS_OPID_REPLY_CACHE;i++)assert(!pthread_create(&workers[i],&attr,hold_identity,(void *)(uintptr_t)i));
    pthread_mutex_lock(&hold_mu);
    while(ready<EFS_OPID_REPLY_CACHE)pthread_cond_wait(&hold_cv,&hold_mu);
    pthread_mutex_unlock(&hold_mu);
    for(unsigned i=0;i<EFS_OPID_REPLY_CACHE;i++)for(unsigned j=0;j<i;j++){
        assert(slots[i]!=slots[j]);assert(concurrent[i].id.seq!=concurrent[j].id.seq);
    }
    unsigned before=calls;
    assert(efs_client_rpc_create(1,1,"test",S_IFREG|0600,0,0,0,NULL,NULL)==EFS_ERR_BUSY);
    assert(efs_client_rpc_unlink(1,1,"test",0)==EFS_ERR_BUSY);
    assert(efs_client_rpc_rename_at(1,1,"test",1,"new",NULL)==EFS_ERR_BUSY);
    assert(efs_client_rpc_link(1,2,1,"test",NULL)==EFS_ERR_BUSY);
    assert(calls==before);
    pthread_mutex_lock(&hold_mu);release_workers=1;pthread_cond_broadcast(&hold_cv);pthread_mutex_unlock(&hold_mu);
    for(unsigned i=0;i<EFS_OPID_REPLY_CACHE;i++)assert(!pthread_join(workers[i],NULL));
    pthread_attr_destroy(&attr);
    assert(efs_client_rpc_unlink(1,1,"test",0)==EFS_OK && calls==before+1);
    puts("16 simultaneous production identity owners: unique slots/sequences, saturation refuses four mutations, released admission recovers PASS");
    /* A later completed burst cannot reuse the oldest live receipt slot.
     * CREATE waits before allocation, then emits exactly one new identity. */
    for(int i=0;i<EFS_OPID_REPLY_CACHE;i++)assert(opid_begin(&held[i])>=0);
    for(int i=1;i<EFS_OPID_REPLY_CACHE;i++)opid_end(i);
    uint64_t admission_next=g_client.opid_next;before=calls;
    pthread_t releaser;assert(!pthread_create(&releaser,NULL,release_oldest,(void *)(intptr_t)0));
    begin=mono_ms();test_deadline=begin+3000;
    assert(efs_client_rpc_create(1,1,"test",S_IFLNK|0777,0,0,0,NULL,NULL)==EFS_OK);
    assert(!pthread_join(releaser,NULL));
    assert(mono_ms()-begin>=1200 && mono_ms()-begin<3000);
    assert(calls==before+1 && last_seq==admission_next && g_client.opid_next==admission_next+1);
    assert(last_ack==admission_next-1);test_deadline=0;
    for(int i=0;i<EFS_OPID_REPLY_CACHE;i++)assert(opid_begin(&held[i])>=0);
    admission_next=g_client.opid_next;before=calls;begin=mono_ms();test_deadline=begin+20;
    assert(efs_client_rpc_create(1,1,"test",S_IFDIR|0700,0,0,0,NULL,NULL)==EFS_ERR_BUSY);
    assert(mono_ms()-begin<200 && calls==before && g_client.opid_next==admission_next);
    for(int i=0;i<EFS_OPID_REPLY_CACHE;i++)opid_end(i);
    test_deadline=0;
    puts("namespace admission: waits before allocation, exact one identity, inherited deadline sends nothing PASS");
    g_client.opid_next=UINT64_MAX;
    begin=mono_ms();test_deadline=begin+3000;
    assert(efs_client_rpc_create(1,1,"test",S_IFDIR|0700,0,0,0,NULL,NULL)==EFS_ERR_BUSY);
    assert(mono_ms()-begin<200 && calls==before);test_deadline=0;
    assert(opid_begin(&q)<0 && !efs_opid_req_valid(&q));
    assert(g_client.opid_next==UINT64_MAX);
    puts("namespace opid saturation: four mutations send nothing, released admission keeps watermark, sequence overflow refused PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-opid-admit-') as work:
    path=Path(work)/'test.c';path.write_text(source)
    binary=path.with_suffix('')
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-I'+str(root/'src/client'),str(path),str(root/'libefs.a'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
