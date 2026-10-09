#!/usr/bin/env python3
"""Production last-close deferral: BUSY cannot strand a later RESOLVE entry."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'src/server/raft_host.c').read_text()
a = source.index('static int host_lease_close_effects(')
function = source[a:source.index('\n}', a)+2]
fixture = r'''
#include "efs/meta_apply.h"
#include "efs/session.h"
#include "efs/lock.h"
#include "efs/txn.h"
#include "efs/kv_key.h"
#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct efs_raft_host {struct efs_kv *kv;pthread_mutex_t wait_mu;};
static int lock_result, append_result, held, marker, writes, signaled;
static uint8_t marker_value[8];
static uint64_t rd64be(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<8;i++)n=n<<8|p[i];return n;}
static void wr64be(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;i++)p[i]=n>>(56-8*i);}
int efs_meta_apply_get_inode(struct efs_kv *kv,efs_ino_t ino,struct efs_meta_row *row)
{(void)kv;memset(row,0,sizeof(*row));row->ino=ino;row->generation=7;return EFS_OK;}
int efs_lease_any(struct efs_kv *kv,efs_ino_t ino,uint64_t gen)
{(void)kv;(void)ino;assert(gen==7);return held;}
int efs_lock_drop_file(struct efs_kv *kv,efs_ino_t ino,uint64_t gen)
{(void)kv;(void)ino;(void)gen;return lock_result;}
int efs_txn_key_exclusive(struct efs_kv *kv,const uint8_t *key,uint32_t len)
{(void)kv;(void)key;(void)len;return 0;}
int efs_meta_apply_append_drain_file(struct efs_kv *kv,efs_ino_t ino)
{(void)kv;(void)ino;return append_result;}
int efs_meta_apply_reclaim(struct efs_kv *kv,efs_ino_t ino)
{(void)kv;(void)ino;return EFS_ERR_BUSY;}
static void lock_wait_signal_locked(struct efs_raft_host *h,efs_ino_t ino)
{(void)h;(void)ino;signaled++;}
static int get(void *ctx,const uint8_t *key,uint32_t len,uint8_t *val,uint32_t *vl)
{(void)ctx;assert(len==11 && key[2]==EFS_KV_KIND_CLOSE_CLEANUP);if(!marker)return EFS_ERR_NOT_FOUND;assert(*vl>=8);memcpy(val,marker_value,8);*vl=8;return 0;}
static int put(void *ctx,const uint8_t *key,uint32_t len,const uint8_t *val,uint32_t vl)
{(void)ctx;(void)key;assert(len==11 && vl==8);memcpy(marker_value,val,8);marker=1;writes++;return 0;}
static int del(void *ctx,const uint8_t *key,uint32_t len)
{(void)ctx;(void)key;assert(len==11);marker=0;return 0;}
''' + function + r'''
int main(void) {
 const struct efs_kv_ops ops={.get=get,.put=put,.del=del};
 struct efs_kv kv={.ops=&ops};struct efs_raft_host h={.kv=&kv};
 assert(!pthread_mutex_init(&h.wait_mu,NULL));
 lock_result=EFS_ERR_BUSY;
 assert(host_lease_close_effects(&h,123,7)==EFS_OK && marker && writes==1 && !signaled);
 assert(rd64be(marker_value)==7);
 /* A storage failure holds the entry; it cannot discard the durable retry. */
 lock_result=EFS_ERR_IO;
 assert(host_lease_close_effects(&h,123,7)==EFS_ERR_IO && marker && writes==1);
 lock_result=EFS_OK;append_result=EFS_ERR_BUSY;
 assert(host_lease_close_effects(&h,123,7)==EFS_OK && marker && writes==2);
 append_result=EFS_OK;
 assert(host_lease_close_effects(&h,123,7)==EFS_OK && !marker && signaled==1);
 assert(!pthread_mutex_destroy(&h.wait_mu));
 puts("close effects: prepared lock/append BUSY persists obligation, IO retains it, resolution drains PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-close-effects-') as temp:
    path=Path(temp);(path/'test.c').write_text(fixture)
    cmd=shlex.split(os.environ.get('CC','cc'))+['-Wall','-Wextra','-I'+str(root/'include'),str(path/'test.c'),str(root/'libefs.a'),'-pthread','-lm','-ldl']
    if os.uname().sysname=='Linux':cmd+=['-libverbs']
    subprocess.run(cmd+['-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
