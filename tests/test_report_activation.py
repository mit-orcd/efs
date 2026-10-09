#!/usr/bin/env python3
"""Delayed REPORT activation must honor the inode session floor at apply."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int apply_activate_lane_cmd(');b=s.index('\n/* EFS_MD_CMD_LANE_FENCE',a)
code=r'''
#include "efs/protocol.h"
#include "efs/meta_apply.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define HOST_ACTIVATE_LANE_LEN 10u
#define HOST_ACTIVATE_MASK_LEN 17u
#define APPLY_LOG(...) ((void)0)
struct efs_raft_host {struct efs_kv *kv;};
static unsigned effects;static int floor_rc;
static int raft_dbg_on(void){return 0;}
static uint32_t rd32be(const uint8_t*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t rd64be(const uint8_t*p){return ((uint64_t)rd32be(p)<<32)|rd32be(p+4);}
uint8_t efs_raft_shard_group(uint32_t shard){assert(shard==efs_kv_inode_shard(1));return 0;}
int efs_session_accept(struct efs_kv*kv,uint32_t sh,const uint8_t*uuid,uint32_t epoch){(void)kv;assert(sh==efs_kv_inode_shard(1) && uuid[0]==9 && epoch==42);return floor_rc;}
int efs_meta_apply_activate_lanes(struct efs_kv*kv,efs_ino_t ino,uint64_t mask){(void)kv;assert(ino==1 && mask==3);effects++;return EFS_OK;}
int efs_meta_apply_activate_lane(struct efs_kv*kv,efs_ino_t ino,uint8_t lane){(void)kv;assert(ino==1 && lane==1);effects++;return EFS_OK;}
''' + s[a:b] + r'''
int main(void){struct efs_raft_host h={0};uint8_t cmd[37]={0};cmd[8]=1;cmd[16]=3;struct efs_opid id={.session_epoch=42};id.client_uuid[0]=9;efs_report_session_pack(cmd+17,&id);
 floor_rc=EFS_ERR_STALE;assert(apply_activate_lane_cmd(&h,cmd,37,1,0)==EFS_ERR_STALE && !effects);
 floor_rc=EFS_ERR_BUSY;assert(apply_activate_lane_cmd(&h,cmd,37,1,0)==EFS_ERR_BUSY && !effects);
 floor_rc=EFS_OK;assert(apply_activate_lane_cmd(&h,cmd,37,1,2)==EFS_ERR_INVAL && !effects);
 assert(apply_activate_lane_cmd(&h,cmd,36,1,0)==EFS_ERR_PROTO && !effects);
 assert(!apply_activate_lane_cmd(&h,cmd,37,1,0) && effects==1);
 assert(!apply_activate_lane_cmd(&h,cmd,17,1,0) && effects==2); /* old logs */
 memset(cmd+9,0,28);cmd[9]=1;efs_report_session_pack(cmd+10,&id);
 floor_rc=EFS_ERR_STALE;assert(apply_activate_lane_cmd(&h,cmd,30,1,0)==EFS_ERR_STALE && effects==2);
 floor_rc=EFS_OK;assert(!apply_activate_lane_cmd(&h,cmd,30,1,0) && effects==3);
 assert(!apply_activate_lane_cmd(&h,cmd,10,1,0) && effects==4);
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-activation-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('REPORT activation: exact single/mask shapes, revoked/missing floors, routing and legacy replay PASS')
