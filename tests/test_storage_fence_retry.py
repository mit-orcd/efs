#!/usr/bin/env python3
"""Run the production idempotent barrier retry with a deterministic I/O clock."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root / 'src/common/session_revoke.c').read_text()
a = s.index('int efs_session_storage_barrier(')
b = s.index('\n}', a) + 2
code = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/session_revoke.h"
static uint64_t now_ms=1000, deadline;
static unsigned calls, busy_count;
static int terminal;
static int fake_clock(clockid_t clock,struct timespec *out) {
 assert(clock==CLOCK_MONOTONIC);out->tv_sec=now_ms/1000;out->tv_nsec=(now_ms%1000)*1000000;return 0;
}
static int fake_sleep(unsigned us) {now_ms+=us/1000;return 0;}
#define clock_gettime fake_clock
#define usleep fake_sleep
static uint64_t efs_net_deadline_ms(void) {return deadline;}
static void efs_net_set_deadline_ms(uint64_t d) {deadline=d;}
static int efs_net_remaining_ms(int cap) {return deadline<=now_ms?0:deadline-now_ms<(uint64_t)cap?(int)(deadline-now_ms):cap;}
static int seed;
static int sess_rpc(const struct efs_session_rpc *rpc,const uint8_t *payload,uint32_t bytes,struct efs_msg_raft_mkfs_reply *out) {
 assert(rpc->ctx==&seed&&bytes==23&&payload[0]==17&&payload[22]==2);
 calls++;out->rc=calls<=busy_count?EFS_ERR_BUSY:terminal;return out->rc;
}
''' + s[a:b] + r'''
int main(void) {
 uint8_t p[23]={17};p[22]=2;struct efs_msg_raft_mkfs_reply out;
 const struct efs_session_rpc rpc={&seed,NULL,0,NULL};
 busy_count=3;terminal=EFS_OK;
 assert(efs_session_storage_barrier(&rpc,p,sizeof(p),&out)==EFS_OK&&calls==4&&deadline==0&&now_ms==1300);
 calls=0;busy_count=0;terminal=EFS_ERR_IO;
 assert(efs_session_storage_barrier(&rpc,p,sizeof(p),&out)==EFS_ERR_IO&&calls==1&&deadline==0);
 calls=0;busy_count=10000;deadline=now_ms+250;
 uint64_t original=deadline;
 assert(efs_session_storage_barrier(&rpc,p,sizeof(p),&out)==EFS_ERR_BUSY&&calls==3&&deadline==original);
 calls=0;deadline=0;now_ms=1000;
 assert(efs_session_storage_barrier(&rpc,p,sizeof(p),&out)==EFS_ERR_BUSY&&calls==150&&deadline==0&&now_ms==16000);
}
'''
with tempfile.TemporaryDirectory(prefix='efs-storage-retry-') as directory:
    p = Path(directory) / 't.c'
    p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'include'), str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
print('storage barrier retry: unchanged identity, bounded BUSY, EIO refusal and prior deadline restoration PASS')
