#!/usr/bin/env python3
"""Committed MKDIR mirrors owe no data REPORT, even under report pressure."""
from pathlib import Path
import os
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('OPS_SOURCE',root/'src/client/ops.c')).read_text()
def function(name):
    a=s.index(name+'(')
    a=s.rfind('\n',0,a)+1
    return s[a:s.index('\n}',a)+2]
code=r'''
#include "client_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct efs_client g_client;
static int pins,locks,marks,admitted,unreserved,pressure,opened,stage_missing;
int efs_client_stage_is_open(efs_ino_t ino){assert(ino==2);return opened;}
int efs_client_stat_local(efs_ino_t ino,struct efs_inode *out){assert(ino==2);memset(out,0,sizeof(*out));return stage_missing?EFS_ERR_NOT_FOUND:EFS_OK;}
static uint64_t now(void){return 42;}
void efs_client_stage_pin(efs_ino_t ino){(void)ino;pins++;}
void efs_client_stage_unpin(efs_ino_t ino){(void)ino;pins--;}
void efs_client_stage_touch(efs_ino_t ino){(void)ino;}
void efs_client_lock_dir(efs_ino_t ino){(void)ino;locks++;}
void efs_client_unlock_dir(efs_ino_t ino){(void)ino;locks--;}
void efs_client_mark_ino_dirty(efs_ino_t ino){assert(ino==1||ino==2);marks|=1<<(ino-1);}
struct efs_export *efs_export_table_for_ino(struct efs_export *ex,efs_ino_t ino){(void)ino;return ex;}
efs_ino_t efs_export_create_with_ino(struct efs_export *ex,efs_ino_t ino,
 efs_ino_t parent,uint32_t mode,uid_t uid,gid_t gid,const char *name){
 (void)ex;(void)ino;(void)parent;(void)mode;(void)uid;(void)gid;(void)name;return 0;
}
int efs_export_upsert_inode(struct efs_export *ex,const struct efs_inode *r){(void)ex;(void)r;return 0;}
int efs_export_set_mtime(struct efs_export *ex,efs_ino_t ino,uint64_t t){(void)ex;(void)ino;(void)t;return 0;}
int efs_client_report_admit(uint64_t records){assert(records==1);admitted++;return pressure?EFS_ERR_BUSY:EFS_OK;}
void efs_client_report_unreserve(void){unreserved++;}
int efs_client_rpc_create(efs_export_id_t ex,efs_ino_t parent,const char *name,
 uint32_t mode,uid_t uid,gid_t gid,uint32_t flags,efs_ino_t *ino,struct efs_inode *out){
 (void)ex;(void)parent;(void)name;(void)uid;(void)gid;(void)flags;(void)ino;
 memset(out,0,sizeof(*out));out->ino=2;out->mode=mode;return EFS_OK;
}
''' + function('create_stage_inode') + '\n' + function('efs_client_create_stage_open_result') + '\n' + function('efs_client_create_result_inode') + r'''
int main(void){
 pthread_mutex_init(&g_client.idx_mu,NULL);
 struct efs_inode out={0};out.ino=2;out.mode=S_IFDIR|0755;
 marks=4; /* Existing unrelated dirty ownership must remain. */
 create_stage_inode(1,"dir",out.mode,1000,1000,&out,1);
 assert(marks==4&&!pins&&!locks);
 out.mode=S_IFREG|0644;create_stage_inode(1,"file",out.mode,1000,1000,&out,1);
 assert(marks==6&&!pins&&!locks);
 pressure=1;marks=4;efs_ino_t ino=0;
 assert(!efs_client_create_result_inode(1,"dir",S_IFDIR|0755,1000,1000,0,&ino,&out));
 assert(ino==2&&S_ISDIR(out.mode)&&marks==4&&!admitted&&!unreserved);
 assert(efs_client_create_result_inode(1,"file",S_IFREG|0644,1000,1000,0,&ino,&out)==EFS_ERR_BUSY);
 assert(!ino&&admitted==1&&!unreserved&&marks==4);
 pressure=0;
 assert(!efs_client_create_result_inode(1,"file",S_IFREG|0644,1000,1000,0,&ino,&out));
 assert(ino==2&&admitted==2&&unreserved==1&&marks==6&&!pins&&!locks);
 struct efs_client_create_request request={.complete=1,.result=EFS_OK,.inode=out};
 request.wire.create.parent=1;request.wire.create.mode=out.mode;
 request.wire.create.uid=1000;request.wire.create.gid=1000;strcpy(request.wire.create.name,"empty");
 pressure=1;marks=4;int old_admitted=admitted;
 assert(efs_client_create_stage_open_result(&request)==EFS_ERR_BUSY&&marks==4&&admitted==old_admitted);
 opened=1;assert(!efs_client_create_stage_open_result(&request)&&marks==4&&admitted==old_admitted&&!pins&&!locks);
 stage_missing=1;assert(efs_client_create_stage_open_result(&request)==EFS_ERR_NOMEM&&marks==4&&!pins&&!locks);stage_missing=0;
 request.complete=0;assert(efs_client_create_stage_open_result(&request)==EFS_ERR_STALE&&marks==4);
 request.complete=1;request.inode.mode=S_IFDIR|0755;assert(efs_client_create_stage_open_result(&request)==EFS_ERR_STALE&&marks==4);
 puts("committed directory stage: no new REPORT ownership, existing marks and regular data admission retained PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-directory-stage-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(['cc','-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
