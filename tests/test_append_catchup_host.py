#!/usr/bin/env python3
"""Actual host functions: one completion proposal, applied verdict before reply."""
import os, re, shlex, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = (root / 'src/server/raft_host.c').read_text()
def function(name):
    begin = source.index('static int ' + name + '(')
    return source[begin:source.index('\n}', begin) + 2]
code = r'''
#include <stdint.h>
#include "efs/meta_cmd.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define EFS_OK 0
#define EFS_ERR_PROTO -7
#define EFS_ERR_INVAL -1
#define EFS_ERR_BUSY -13
#define EFS_ERR_STALE -14
#define EFS_ERR_IO -6
#define EFS_ERR_NOT_FOUND -2
#define EFS_OPID_UUID_LEN 16
#define EFS_KV_KEY_MAX 64
#define EFS_META_LANES 64
#define EFS_META_APPEND_CATCHUP_MAX 32766
#define EFS_ERR_OVERFLOW -16
#define EFS_META_APPEND_COMPLETE_MAX 8
#define EFS_TXN_MAX_PART 144
#define EFS_OPID_VAL_MAX 1024
#define EFS_LOCK_CAP 128
#define EFS_SESSION_CLEANUP_MAX 32
#define EFS_CLOSE_STEP_VALUE 36
#define EFS_META_GC_VAL 80
#define EFS_META_TRUNCATE_ITEMS 16384
#define EFS_META_LANE_FENCE_ITEMS 16384
#define EFS_TXN_RESOLVE_ITEMS 48
#define EFS_TXN_DROP_ITEMS 32
#define EFS_TXN_RECORD_MAX 1024
struct efs_kv_item {int op;const uint8_t *key;uint32_t klen;const uint8_t *val;uint32_t vlen;};
typedef uint64_t efs_ino_t;
struct efs_opid {uint8_t client_uuid[16];uint32_t session_epoch;};
struct efs_meta_row {uint64_t generation,content_epoch;};
struct efs_txid {int id;};
struct efs_raft_host {void *kv;};
struct host_resize_ctx {struct efs_raft_host *host;int *hint;};
struct host_append_reserve_ctx {struct host_resize_ctx base;int leading;};
static int host_append_reserve_ops;
static uint64_t watermark=80000;
static uint32_t remaining=20000, completed, proposals, retirements, authorities;
static int state_error,authority_error,inode_error,proposal_error,retirement_error,metadata_error;
static uint8_t captured[58];static uint32_t captured_length;
static unsigned apply_calls;
static uint64_t rd64be(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
static uint32_t rd32be(const uint8_t *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v=(v<<8)|p[i];return v;}
static void wr64be(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=v>>(56-8*i);}
static void wr32be(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(24-8*i);}
static uint32_t efs_kv_inode_shard(efs_ino_t ino){assert(ino==11);return 1;}
static uint8_t efs_raft_shard_group(uint32_t shard){assert(shard==1);return 2;}
static int efs_meta_apply_append_state(void *kv,efs_ino_t ino,uint64_t *wm,uint64_t *frontier,uint32_t *open){
 (void)kv;assert(ino==11&&!frontier);if(wm)*wm=watermark;*open=remaining;return state_error;
}
static int host_append_authority(struct efs_raft_host *host,int *hint){(void)host;(void)hint;authorities++;return authority_error;}
static int efs_meta_apply_get_inode(void *kv,efs_ino_t ino,struct efs_meta_row *row){
 (void)kv;assert(ino==11);row->generation=7;row->content_epoch=9;return inode_error;
}
static int host_propose_wait(struct efs_raft_host *host,uint8_t group,const uint8_t *cmd,uint32_t len,int *hint){
 (void)host;(void)hint;assert(group==2&&cmd[0]==EFS_MD_CMD_APPEND_CATCHUP&&(len==38||len==58));proposals++;
 memcpy(captured,cmd,len);captured_length=len;
 assert(rd64be(cmd+1)==11&&rd64be(cmd+9)==7&&rd64be(cmd+17)==9);
 if(proposal_error)return proposal_error;
 uint32_t n=rd64be(cmd+25)/4;if(n>20000)n=20000;
 if(n>completed){remaining-=n-completed;completed=n;}
 return 0;
}
static void fill_txid(struct efs_raft_host *host,struct efs_txid *tx){(void)host;tx->id=19;}
static int efs_meta_retire_append_barriers(void *kv,efs_ino_t ino,const struct efs_opid *owner,
 const struct efs_txid *tx,const void *ops,void *user){
 (void)kv;struct host_append_reserve_ctx *c=user;
 assert(ino==11&&tx->id==19&&ops==&host_append_reserve_ops&&!remaining);
 assert(c->leading==(owner?2:1));retirements++;
 if(!retirement_error)watermark=0;
 return retirement_error;
}
static int efs_meta_apply_append_catchup(void *kv,efs_ino_t ino,uint64_t gen,uint64_t epoch,uint64_t size,uint32_t limit,const struct efs_opid *owner){
 (void)kv;assert(ino==11&&gen==7&&epoch==9&&size==80000&&limit==20000);
 if(owner)assert(owner->client_uuid[0]==42&&owner->session_epoch==3);
 apply_calls++;return metadata_error;
}
''' + source[source.index('static uint64_t host_pressure_add('):source.index('static int host_pressure_recovery(')] + function('host_resolve_caught_up') + '\n' + function('apply_append_catchup_cmd') + r'''
static void reset(void){
 watermark=80000;remaining=20000;completed=proposals=retirements=authorities=apply_calls=0;
 state_error=authority_error=inode_error=proposal_error=retirement_error=metadata_error=0;
}
int main(void){
 struct efs_raft_host h={0};int hint=-1;struct efs_opid id={.client_uuid={42},.session_epoch=3};
 for(unsigned owned=0;owned<2;owned++){
  reset();assert(!host_resolve_caught_up(&h,11,80000,owned?&id:NULL,&hint));
  assert(proposals==1&&completed==20000&&retirements==1&&authorities==1&&!watermark);
  assert(captured_length==(owned?58u:38u)&&captured[37]==owned);
  assert(!apply_append_catchup_cmd(&h,captured,captured_length,2)&&apply_calls==1);
  assert(apply_append_catchup_cmd(&h,captured,captured_length,0)==EFS_ERR_INVAL&&apply_calls==1);
  assert(apply_append_catchup_cmd(&h,captured,captured_length-1,2)==EFS_ERR_PROTO&&apply_calls==1);
  captured[37]^=1;assert(apply_append_catchup_cmd(&h,captured,captured_length,2)==EFS_ERR_PROTO&&apply_calls==1);captured[37]^=1;
  metadata_error=EFS_ERR_IO;assert(apply_append_catchup_cmd(&h,captured,captured_length,2)==EFS_ERR_IO&&apply_calls==2);
  reset();assert(!host_resolve_caught_up(&h,11,40000,owned?&id:NULL,&hint));
  assert(proposals==1&&completed==10000&&remaining==10000&&!retirements);
  reset();proposal_error=EFS_ERR_STALE;assert(host_resolve_caught_up(&h,11,80000,owned?&id:NULL,&hint)==EFS_ERR_BUSY&&!completed&&!retirements);
  reset();proposal_error=EFS_ERR_IO;assert(host_resolve_caught_up(&h,11,80000,owned?&id:NULL,&hint)==EFS_ERR_IO&&!completed&&!retirements);
  reset();retirement_error=EFS_ERR_IO;assert(host_resolve_caught_up(&h,11,80000,owned?&id:NULL,&hint)==EFS_ERR_IO&&completed==20000&&proposals==1);
  retirement_error=0;assert(!host_resolve_caught_up(&h,11,80000,owned?&id:NULL,&hint)&&proposals==1&&retirements==2);
  reset();authority_error=EFS_ERR_BUSY;assert(host_resolve_caught_up(&h,11,80000,owned?&id:NULL,&hint)==EFS_ERR_BUSY&&!proposals);
  reset();inode_error=EFS_ERR_NOT_FOUND;assert(host_resolve_caught_up(&h,11,80000,owned?&id:NULL,&hint)==EFS_ERR_BUSY&&!proposals);
  reset();state_error=EFS_ERR_IO;assert(host_resolve_caught_up(&h,11,80000,owned?&id:NULL,&hint)==EFS_ERR_IO&&!proposals);
  reset();watermark=0;assert(!host_resolve_caught_up(&h,11,80000,owned?&id:NULL,&hint)&&!proposals&&!authorities);
 }
 uint8_t charge_cmd[38]={EFS_MD_CMD_APPEND_CATCHUP};
 wr32be(charge_cmd+33,1);uint64_t small=host_pressure_command_charge(charge_cmd,38);
 wr32be(charge_cmd+33,20000);uint64_t large=host_pressure_command_charge(charge_cmd,38);
 wr32be(charge_cmd+33,EFS_META_APPEND_CATCHUP_MAX);uint64_t maximum=host_pressure_command_charge(charge_cmd,38);
 assert(small<16384&&large>small&&maximum>large);
 assert(host_pressure_command_charge(charge_cmd,37)>=maximum);
 puts("append catchup host: one completion proposal for 20k, strict route/wire, failed verdicts, partial REPORT and interrupted retirement retry PASS");
}
'''
# The combined namespace admission charge uses the real session row size.
value=re.search(r'^#define\s+EFS_SESSION_VAL\s+(\d+)',(root/'include/efs/session.h').read_text(),re.M)
assert value
code='#define EFS_SESSION_VAL '+value.group(1)+'\n'+code
with tempfile.TemporaryDirectory(prefix='efs-catchup-host-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(code)
    binary = path.with_suffix('')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-I'+str(root/'include'), '-std=c99', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
