#!/usr/bin/env python3
"""Committed directory mirrors owe no new REPORT; existing dirty work survives."""
from pathlib import Path
import os
import subprocess
import tempfile
import shlex
import shutil
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
#include <time.h>
#include <unistd.h>
struct efs_client g_client;
static __thread uint64_t call_deadline;
uint64_t efs_client_rpc_deadline_ms(void){return call_deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t value){call_deadline=value;}
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

static int rename_status,rename_calls,rename_mode=S_IFDIR|0755,local_missing,forgot,refreshed,refresh_failure;
#define inherited_deadline call_deadline /* devel already stubs the deadline */
int efs_client_rpc_past_deadline(void){return 0;}
int efs_client_rpc_rename_at(efs_export_id_t ex,efs_ino_t parent,const char *name,
 efs_ino_t dest,const char *newname,struct efs_inode *out){
 (void)ex;(void)parent;(void)dest;(void)name;(void)newname;
 assert(pins==3&&!locks);rename_calls++;
 memset(out,0,sizeof(*out));out->ino=2;out->mode=rename_mode;
 return rename_status;
}
void efs_client_lock_dirs2(efs_ino_t a,efs_ino_t b){(void)a;(void)b;locks++;}
void efs_client_unlock_dirs2(efs_ino_t a,efs_ino_t b){(void)a;(void)b;locks--;}
int efs_export_rename_at(struct efs_export *ex,efs_ino_t parent,const char *name,
 efs_ino_t dest,const char *newname){
 (void)ex;(void)parent;(void)dest;(void)name;(void)newname;assert(pins==3&&locks==1);
 return local_missing?EFS_ERR_NOT_FOUND:EFS_OK;
}
int efs_export_rename(struct efs_export *ex,efs_ino_t ino,efs_ino_t parent,const char *name){
 (void)ex;(void)ino;(void)parent;(void)name;assert(pins==3&&locks==1);return EFS_ERR_NOT_FOUND;
}
void efs_export_forget_name(struct efs_export *ex,efs_ino_t parent,const char *name,efs_ino_t ino){
 (void)ex;(void)parent;(void)name;assert(ino==2&&pins==3&&locks==1);forgot++;
}
int efs_client_stat_refresh(efs_ino_t ino,struct efs_inode *out){
 (void)ino;assert(pins==3&&!locks);memset(out,0,sizeof(*out));refreshed++;
 return refresh_failure?EFS_ERR_IO:EFS_OK;
}
''' + function('create_stage_inode') + '\n' + function('efs_client_create_stage_open_result') + '\n' + function('efs_client_create_result_inode') + '\n' + function('efs_client_rename_at') + r'''
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

 /* A successful directory namespace RPC already committed its metadata.
  * Its clean mirror must not manufacture REPORT ownership, while unrelated
  * and same-inode pending ownership must never be cleared. */
 marks=4;inherited_deadline=37;
 assert(!efs_client_rename_at(2,1,"dir",3,"moved"));
 assert(marks==4&&!pins&&!locks&&rename_calls==1&&forgot==1&&refreshed==2&&inherited_deadline==37);
 marks=6;local_missing=1;refresh_failure=1;
 assert(!efs_client_rename_at(2,1,"dir",3,"moved"));
 assert(marks==6&&!pins&&!locks&&rename_calls==2&&forgot==2&&refreshed==4&&inherited_deadline==37);
 /* Same-name/no-op and same-parent moves obey the same ownership rule. */
 marks=4;local_missing=0;refresh_failure=0;
 assert(!efs_client_rename_at(2,1,"dir",1,"dir"));
 assert(marks==4&&!pins&&!locks&&forgot==2&&refreshed==5);
 /* An RPC refusal cannot mutate local ownership or mirror state. */
 rename_status=EFS_ERR_INVAL;marks=4;
 assert(efs_client_rename_at(2,1,"dir",3,"moved")==EFS_ERR_INVAL);
 assert(marks==4&&!pins&&!locks&&forgot==2&&refreshed==5&&inherited_deadline==37);
 /* Regular-file staging protection remains unchanged. */
 rename_status=0;rename_mode=S_IFREG|0644;marks=4;
 assert(!efs_client_rename_at(2,1,"file",3,"moved"));
 assert(marks==6&&!pins&&!locks&&forgot==2&&refreshed==7);
 puts("committed directory stage: MKDIR/RENAME add no REPORT; existing marks, failure cleanup and regular data admission retained PASS");
}
'''
d=Path(tempfile.mkdtemp(prefix='efs-directory-stage-'))
try:
    p=d/'test.c';p.write_text(code)
    flags=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread']
    if os.environ.get('EFS_TEST_SANITIZE')=='1':
        flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
    subprocess.run(flags+['-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
except BaseException:
    print('FAILED directory-stage source/binary retained:',d,flush=True)
    raise
else:
    shutil.rmtree(d)
