#!/usr/bin/env python3
"""Actual xattr callbacks reject empty user namespace without issuing RPCs."""
import os,re,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_FUSE_SOURCE',root/'src/client/efs_fuse.c')).read_text()
def function(name):
 m=re.search(r'^static (?:int|void) '+name+r'\([^;]+?\)\n\{',s,re.M)
 return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <efs/metadata.h>
#include <efs/meta_cmd.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
typedef uint64_t fuse_ino_t;
typedef void *fuse_req_t;
static fuse_req_t t_req;
static struct {uint64_t export_id;} g_client;
static int depth,replied=-1,calls,mutations,rpc_rc;
static int virt_kind(uint64_t ino){(void)ino;return 0;}
static void lookup_memo_mutation_begin(void){assert(!depth);depth++;}
static void lookup_memo_mutation_end(void){assert(depth==1);depth--;}
static void fuse_reply_err(fuse_req_t req,int err){(void)req;assert(err>=0);assert(!depth&&!t_req);replied=err;}
static void fuse_reply_xattr(fuse_req_t req,size_t n){(void)req;assert(n==1&&!t_req);replied=0;}
static void fuse_reply_buf(fuse_req_t req,const char *buf,size_t n){(void)req;assert(n==1&&buf[0]=='x'&&!t_req);replied=0;}
static int efs_client_rpc_xattr(uint64_t export_id,uint64_t ino,uint8_t op,uint32_t flags,const char *name,uint16_t nlen,const void *value,uint32_t vlen,void *out,uint32_t *nbytes){
 (void)export_id;(void)ino;(void)flags;(void)value;(void)vlen;assert(name&&nlen==strlen(name));calls++;if(op==EFS_XATTR_SET||op==EFS_XATTR_REMOVE)mutations++;if(out){((char*)out)[0]='x';*nbytes=1;}return rpc_rc;
}
'''+function('efs_rc_to_errno')+function('xattr_user_name')+function('ll_setxattr_run')+function('ll_getxattr')+function('ll_removexattr_run')+r'''
static void refused(const char *name,int error){
 int before=calls;fuse_req_t req=(void*)(uintptr_t)1;
 ll_setxattr_run(req,42,name,"x",1,0);assert(replied==error);
 ll_getxattr(req,42,name,1);assert(replied==error);
 ll_removexattr_run(req,42,name);assert(replied==error);
 assert(calls==before&&!depth&&!t_req);
}
int main(void){
 refused("user.",EINVAL);refused("",EINVAL);refused(NULL,EINVAL);
 refused("security.label",EOPNOTSUPP);refused("system.label",EOPNOTSUPP);refused("invalid",EOPNOTSUPP);
 char oversized[257];memset(oversized,'a',sizeof(oversized));memcpy(oversized,"user.",5);oversized[256]=0;refused(oversized,ERANGE);
 char boundary[256];memset(boundary,'a',sizeof(boundary));memcpy(boundary,"user.",5);boundary[255]=0;assert(!xattr_user_name(boundary));
 fuse_req_t req=(void*)(uintptr_t)1;
 ll_setxattr_run(req,42,"user.efs","x",1,0);assert(!replied&&calls==1&&mutations==1);
 ll_getxattr(req,42,"user.efs",0);assert(!replied&&calls==2);
 ll_getxattr(req,42,"user.efs",1);assert(!replied&&calls==3);
 ll_removexattr_run(req,42,"user.efs");assert(!replied&&calls==4&&mutations==2);
 rpc_rc=EFS_ERR_NODATA;ll_getxattr(req,42,"user.missing",1);assert(replied==ENODATA);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-xattr-names-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('Xattr names: empty user namespace refused before RPC; ordinary names, size boundary, errors and balanced callbacks PASS')
