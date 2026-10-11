#!/usr/bin/env python3
"""Captured recovery cursors may replace markers only after their KV barrier."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
def fn(signature):
 a=s.index('static '+signature);return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/kv.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define HOST_NGROUPS 2
struct efs_raft_host {struct efs_kv *kv;struct {uint8_t group;uint64_t cursor_durable;} g[2];};
static int result;static uint32_t bytes;static uint8_t raw[16];
static void wr64be(uint8_t*p,uint64_t v){for(int i=7;i>=0;i--){p[i]=v;v>>=8;}}
static uint64_t rd64be(const uint8_t*p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=v<<8|p[i];return v;}
static int fixture_get(struct efs_kv*kv,const uint8_t*k,uint32_t kl,uint8_t*v,uint32_t*vl){
 (void)kv;assert(kl==7 && k[0]==255 && k[1]==255 && (k[6]==0 || k[6]==2));
 if(result)return result;
 assert(*vl==16);memcpy(v,raw,bytes);*vl=bytes;return 0;
}
#define efs_kv_get fixture_get
'''+fn('void host_kv_cursor(')+fn('uint64_t host_capture_cursor(')+fn('void host_publish_cursor(')+r'''
static void set(uint64_t idx){wr64be(raw,idx);wr64be(raw+8,~idx);bytes=16;result=0;}
int main(void){
 struct efs_raft_host h={0};h.g[1].group=2;uint64_t captured[2];
 set(80);assert(host_capture_cursor(&h,0,80)==80);
 assert(host_capture_cursor(&h,0,81)==80); /* no-op above cursor */
 assert(!host_capture_cursor(&h,0,79));assert(!host_capture_cursor(&h,0,0));
 for(int group=0;group<2;group++)captured[group]=host_capture_cursor(&h,group,80);
 set(90); /* Another later memory image is not the barrier's captured proof. */
 host_publish_cursor(&h,captured,EFS_ERR_IO);
 assert(!h.g[0].cursor_durable && !h.g[1].cursor_durable);
 host_publish_cursor(&h,captured,EFS_OK);
 assert(h.g[0].cursor_durable==80 && h.g[1].cursor_durable==80);
 captured[0]=captured[1]=90;host_publish_cursor(&h,captured,EFS_ERR_NOMEM);
 assert(h.g[0].cursor_durable==80 && h.g[1].cursor_durable==80);
 raw[15]^=1;assert(!host_capture_cursor(&h,0,90));set(90);bytes=8;
 assert(!host_capture_cursor(&h,0,90));
 int errors[]={EFS_ERR_NOT_FOUND,EFS_ERR_IO,EFS_ERR_NOMEM,EFS_ERR_BUSY};
 for(unsigned i=0;i<sizeof(errors)/sizeof(*errors);i++){result=errors[i];assert(!host_capture_cursor(&h,1,90));}
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-cursor-checkpoint-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 flags=['-fsanitize=address,undefined'] if os.environ.get('EFS_TEST_SANITIZE')=='1' else []
 subprocess.run(['cc',*flags,'-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
a=s.index('static void *host_pump(');b=s.index('\n}',a);pump=s[a:b]
assert pump.index('host_capture_cursor(')<pump.index('pthread_mutex_unlock(&h->mu)')
assert pump.index('efs_kv_lsm_sync_release(')<pump.index('host_publish_cursor(')<pump.index('host_checkpoint_dispatch(')
print('cursor checkpoint: validated captured prefix, successful barrier only, later writes and no-op fallback PASS')
