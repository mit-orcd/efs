#!/usr/bin/env python3
"""The production REPORT envelope rejects fenced/malformed batches before effects."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int apply_report_publish_cmd(');b=s.index('\nstatic int apply_publish_cmd(',a)
code=r'''
#include "efs/protocol.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define HOST_PUBLISH_LEN 202u
#define HOST_REPORT_PREFIX_LEN 21u
struct efs_raft_host { struct efs_kv *kv; };
static unsigned effects,holds,releases;static int floor_rc,publish_rc,release_rc;
static uint32_t rd32be(const uint8_t*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t rd64be(const uint8_t*p){return ((uint64_t)rd32be(p)<<32)|rd32be(p+4);}
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;return lane;}
uint8_t efs_raft_shard_group(uint32_t shard){return shard&2;}
int efs_session_accept(struct efs_kv*kv,uint32_t shard,const uint8_t*uuid,uint32_t epoch){(void)kv;assert(uuid[0]==9 && epoch==42);return shard==1?floor_rc:EFS_OK;}
static void unpack_publication(const uint8_t*p,struct efs_meta_pub*out,uint64_t idx,uint64_t term,uint8_t group){(void)idx;(void)term;(void)group;memset(out,0,sizeof(*out));out->ino=rd64be(p+1);out->chunk_index=rd32be(p+9);}
int efs_meta_apply_publish(struct efs_kv*kv,const struct efs_meta_pub*p){(void)kv;(void)p;effects++;return publish_rc;}
int efs_kv_lsm_sync_hold(struct efs_kv*kv){(void)kv;holds++;return EFS_OK;}
int efs_kv_lsm_sync_release(struct efs_kv*kv){(void)kv;releases++;return release_rc;}
''' + s[a:b] + r'''
int main(void){struct efs_raft_host h={0};uint8_t cmd[21+404]={EFS_MD_CMD_REPORT_PUBLISH};struct efs_opid id={.session_epoch=42};id.client_uuid[0]=9;efs_report_session_pack(cmd+1,&id);cmd[21]=cmd[223]=EFS_MD_CMD_PUBLISH;cmd[21+8]=cmd[223+8]=1;cmd[223+12]=1;
 floor_rc=EFS_ERR_STALE;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_STALE && !effects && !holds);
 floor_rc=EFS_ERR_BUSY;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_BUSY && !effects);
 floor_rc=EFS_ERR_NOT_FOUND;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_NOT_FOUND && !effects);
 floor_rc=EFS_OK;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd)-1,0)==EFS_ERR_PROTO && !effects);
 cmd[223]=0xff;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_PROTO && !effects);cmd[223]=EFS_MD_CMD_PUBLISH;
 cmd[223+12]=2;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_INVAL && !effects);cmd[223+12]=1;
 assert(!apply_report_publish_cmd(&h,cmd,sizeof(cmd),0) && effects==2 && holds==1 && releases==1);
 publish_rc=EFS_ERR_IO;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_IO && effects==3 && holds==2 && releases==2);
 publish_rc=EFS_OK;release_rc=EFS_ERR_IO;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_IO && effects==5 && holds==3 && releases==3);
 memset(cmd+1,0,16);assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_INVAL && effects==5);
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-session-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('REPORT apply: whole-frame validation, strict missing/fenced floors, lane routing and WAL failures PASS')
