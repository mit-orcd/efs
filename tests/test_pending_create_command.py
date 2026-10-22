#!/usr/bin/env python3
"""Daemon pending CREATE encoding/apply preserves identity and rejects malformed input."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
def fn(name):
 a=s.index('static '+name);return s[a:s.index('\n}',a)+2]
o=(root/'src/sim/opid.c').read_text();a=o.index('int efs_opid_req_valid(');valid=o[a:o.index('\n}',a)+2]
code=r'''
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include "efs/opid.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#define HOST_CREATE_NAME_OFF 31
#define HOST_CREATE_F_OPID 1
#define HOST_CREATE_F_PENDING 2
#define HOST_OPID_TRAILER 36
#define HOST_CMD_MAX 1024
#define APPLY_LOG(...) ((void)0)
struct efs_raft_host {struct efs_kv *kv;};
static unsigned pending_calls,legacy_calls;
static uint64_t rd64be(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=v<<8|p[i];return v;}
static uint32_t rd32be(const uint8_t *p){return (uint32_t)rd64be((uint8_t[8]){0,0,0,0,p[0],p[1],p[2],p[3]});}
static void wr64be(uint8_t *p,uint64_t v){for(int i=7;i>=0;i--){p[i]=v;v>>=8;}}
static void wr32be(uint8_t *p,uint32_t v){for(int i=3;i>=0;i--){p[i]=v;v>>=8;}}
static void host_hold_uuid(uint64_t owner,uint8_t *p){wr64be(p,owner);}
int efs_meta_apply_create_file_log_pending(struct efs_kv *kv,const struct efs_meta_attrs *at,efs_ino_t parent,uint32_t mode,const char *name,efs_ino_t ino,int layout,const struct efs_opid_req *q,const uint8_t *uuid,uint32_t epoch,efs_ino_t *out) {
 (void)kv;(void)at;assert(parent==1&&mode==(S_IFREG|0600)&&!strcmp(name,"p")&&ino==4097&&layout==0);
 assert(epoch==7&&q&&q->id.seq==10&&q->ack==9&&uuid[0]==21&&uuid[15]==230);pending_calls++;*out=ino;return EFS_OK;
}
int efs_meta_apply_create_file_log_lease(struct efs_kv *kv,const struct efs_meta_attrs *at,efs_ino_t parent,uint32_t mode,const char *name,efs_ino_t ino,int layout,const struct efs_opid_req *q,const uint8_t *uuid,uint32_t epoch,efs_ino_t *out) {
 (void)kv;(void)at;(void)parent;(void)mode;(void)name;(void)layout;(void)q;
 assert(uuid&&epoch==1&&rd64be(uuid)==123);legacy_calls++;*out=ino;return EFS_OK;
}
int efs_meta_apply_mkdir_log_op(struct efs_kv *kv,const struct efs_meta_attrs *at,efs_ino_t parent,uint32_t mode,const char *name,efs_ino_t ino,int layout,const struct efs_opid_req *q,efs_ino_t *out) {assert(0);return EFS_ERR_IO;}
int efs_meta_apply_mkdir(struct efs_kv *kv,const struct efs_meta_attrs *at,efs_ino_t parent,uint32_t mode,const char *name,efs_ino_t *out) {assert(0);return EFS_ERR_IO;}
'''+valid+fn('uint32_t pack_opid_trailer')+fn('void unpack_opid_trailer')+fn('int pack_create_cmd')+fn('int apply_create_cmd')+r'''
int main(void) {
 uint8_t bytes[1024],bad[1024];uint32_t n;struct efs_meta_attrs at={0};struct efs_raft_host host={0};
 struct efs_opid_req q={.id={.client_uuid={21,[15]=230},.session_epoch=7,.seq=10},.ack=9};
 assert(!pack_create_cmd(bytes,&n,1,S_IFREG|0600,"p",&at,0,4097,0,&q,1));
 assert(!apply_create_cmd(&host,bytes,n,1)&&pending_calls==1);
 assert(!apply_create_cmd(&host,bytes,n,2)&&pending_calls==2);
 for(uint32_t length=2;length<n;length++){int rc=apply_create_cmd(&host,bytes,length,3);assert(rc==EFS_ERR_PROTO||rc==EFS_ERR_UNSUPPORTED);}
 memcpy(bad,bytes,n);bad[1]|=128;assert(apply_create_cmd(&host,bad,n,3)==EFS_ERR_UNSUPPORTED);
 memcpy(bad,bytes,n);bad[1]&=~HOST_CREATE_F_OPID;assert(apply_create_cmd(&host,bad,n,3)==EFS_ERR_UNSUPPORTED);
 memcpy(bad,bytes,n);bad[32+19]=8;assert(apply_create_cmd(&host,bad,n,3)==EFS_ERR_PROTO);
 memcpy(bad,bytes,n);bad[32+15]^=1;assert(apply_create_cmd(&host,bad,n,3)==EFS_ERR_PROTO);
 assert(pending_calls==2);
 assert(pack_create_cmd(bytes,&n,1,S_IFREG|0600,"p",&at,123,4097,0,&q,1)==EFS_ERR_INVAL);
 assert(pack_create_cmd(bytes,&n,1,S_IFREG|0600,"p",&at,0,4097,0,NULL,1)==EFS_ERR_INVAL);
 assert(!pack_create_cmd(bytes,&n,1,S_IFREG|0600,"p",&at,123,4097,0,&q,0));
 assert(!apply_create_cmd(&host,bytes,n,4)&&legacy_calls==1);
 puts("pending CREATE command: full UUID/epoch/token, strict truncation/identity rejection and historical lease replay PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pending-command-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
