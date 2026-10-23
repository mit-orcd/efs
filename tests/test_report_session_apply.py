#!/usr/bin/env python3
"""The production REPORT envelope rejects fenced/malformed batches before effects."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static void unpack_report_ticket(');b=s.index('\n/* Same encoding as sim_txn_apply.',a)
code=r'''
#include "efs/protocol.h"
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#define HOST_PUBLISH_LEN 202u
#define HOST_REPORT_PREFIX_LEN 21u
#define HOST_PUB_F_LANE_LOCAL 1
#define HOST_PUB_F_FRESH_OBJECT 2
#define HOST_PUB_F_TICKETED 4
#define HOST_PUB_F_REJECT_GONE 16
#define HOST_REPORT_REC_LEN (HOST_PUBLISH_LEN + 16u)
struct efs_raft_host { struct efs_kv *kv; };
static unsigned effects,holds,releases,rejects;static int floor_rc,publish_rc,release_rc;
static uint32_t rd32be(const uint8_t*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t rd64be(const uint8_t*p){return ((uint64_t)rd32be(p)<<32)|rd32be(p+4);}
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;return lane;}
uint8_t efs_raft_shard_group(uint32_t shard){return shard&2;}
int efs_session_accept(struct efs_kv*kv,uint32_t shard,const uint8_t*uuid,uint32_t epoch){(void)kv;assert(uuid[0]==9 && epoch==42);return shard==1?floor_rc:EFS_OK;}
static void unpack_publication(const uint8_t*p,struct efs_meta_pub*out,uint64_t idx,uint64_t term,uint8_t group){(void)idx;(void)term;(void)group;memset(out,0,sizeof(*out));out->ino=rd64be(p+1);out->chunk_index=rd32be(p+9);}
int efs_meta_apply_publish(struct efs_kv*kv,const struct efs_meta_pub*p){(void)kv;assert(p->ticketed && p->put_id.seq==77 && p->put_id.session_epoch==42 && p->put_id.client_uuid[0]==9 && p->put_fragment_len==65536 && p->put_member_mask==7);effects++;return publish_rc;}
int efs_meta_apply_publish_reject(struct efs_kv*kv,const struct efs_meta_pub*p,int verdict){(void)kv;assert(p->ticketed && !verdict);rejects++;return EFS_OK;}
static void wr32be(uint8_t*p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
int efs_kv_lsm_sync_hold(struct efs_kv*kv){(void)kv;holds++;return EFS_OK;}
int efs_kv_lsm_sync_release(struct efs_kv*kv){(void)kv;releases++;return release_rc;}
''' + s[a:b] + r'''
int main(void){struct efs_raft_host h={0};enum{R0=21,R1=21+HOST_REPORT_REC_LEN};uint8_t cmd[21+2*HOST_REPORT_REC_LEN]={EFS_MD_CMD_REPORT_PUBLISH};
 struct efs_opid id={.session_epoch=42};id.client_uuid[0]=9;efs_report_session_pack(cmd+1,&id);
 for(unsigned r=R0;r<sizeof(cmd);r+=HOST_REPORT_REC_LEN){cmd[r]=EFS_MD_CMD_PUBLISH;cmd[r+8]=1;cmd[r+HOST_PUBLISH_LEN-1]=HOST_PUB_F_TICKETED|HOST_PUB_F_FRESH_OBJECT;
  memset(cmd+r+HOST_PUBLISH_LEN,0,8);cmd[r+HOST_PUBLISH_LEN+7]=77;wr32be(cmd+r+HOST_PUBLISH_LEN+8,65536);wr32be(cmd+r+HOST_PUBLISH_LEN+12,7);}
 cmd[R1+12]=1;
 floor_rc=EFS_ERR_STALE;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_STALE && !effects && !holds);
 floor_rc=EFS_ERR_BUSY;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_BUSY && !effects);
 floor_rc=EFS_ERR_NOT_FOUND;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_NOT_FOUND && !effects);
 floor_rc=EFS_OK;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd)-1,0)==EFS_ERR_PROTO && !effects);
 cmd[R1]=0xff;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_PROTO && !effects);cmd[R1]=EFS_MD_CMD_PUBLISH;
 cmd[R1+12]=2;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_INVAL && !effects);cmd[R1+12]=1;
 /* K13: an unticketed record (or one without the fresh-object flag, or an
  * unknown flag) refuses the whole frame before any effect. */
 cmd[R1+HOST_PUBLISH_LEN-1]=HOST_PUB_F_FRESH_OBJECT;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_PROTO && !effects);
 cmd[R1+HOST_PUBLISH_LEN-1]=HOST_PUB_F_TICKETED;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_PROTO && !effects);
 cmd[R1+HOST_PUBLISH_LEN-1]=HOST_PUB_F_TICKETED|HOST_PUB_F_FRESH_OBJECT|8;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_PROTO && !effects);
 cmd[R1+HOST_PUBLISH_LEN-1]=HOST_PUB_F_TICKETED|HOST_PUB_F_FRESH_OBJECT;
 assert(!apply_report_publish_cmd(&h,cmd,sizeof(cmd),0) && effects==2 && holds==1 && releases==1);
 /* A GONE record settles its ticket only. */
 cmd[R1+HOST_PUBLISH_LEN-1]|=HOST_PUB_F_REJECT_GONE;assert(!apply_report_publish_cmd(&h,cmd,sizeof(cmd),0) && effects==3 && rejects==1);
 cmd[R1+HOST_PUBLISH_LEN-1]&=(uint8_t)~HOST_PUB_F_REJECT_GONE;
 publish_rc=EFS_ERR_IO;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_IO && effects==4 && holds==3 && releases==3);
 publish_rc=EFS_OK;release_rc=EFS_ERR_IO;assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_IO && effects==6 && holds==4 && releases==4);
 memset(cmd+1,0,16);assert(apply_report_publish_cmd(&h,cmd,sizeof(cmd),0)==EFS_ERR_INVAL && effects==6);
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-session-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('REPORT apply: whole-frame validation, ticket-only records, GONE settle, strict missing/fenced floors, lane routing and WAL failures PASS')
