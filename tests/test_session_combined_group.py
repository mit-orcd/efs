#!/usr/bin/env python3
"""The combined two-row admission must never enter another Raft group."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_session_combined_group(');b=s.index('\n}',a)+2
code=r'''
#include "efs/common.h"
#include "efs/raft.h"
#include "efs/session.h"
#include <assert.h>
#include <stdint.h>
#define HOST_SESS_REG_LEN 26
static uint32_t owner=1;
static uint32_t efs_kv_session_shard(const uint8_t *uuid){(void)uuid;return owner;}
static uint32_t rd32be(const uint8_t*p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put(uint8_t*p,uint32_t n){for(int i=3;i>=0;i--){p[i]=n;n>>=8;}}
'''+s[a:b]+r'''
int main(void){uint8_t cmd[26]={0};
 for(owner=1;owner<=2;owner++)for(uint32_t shard=1;shard<64;shard++)for(unsigned group=0;group<3;group++){
  put(cmd+22,shard);int same=group==efs_raft_shard_group(owner) && group==efs_raft_shard_group(shard);
  assert(host_session_combined_group(cmd,26,group)==(same?EFS_OK:EFS_ERR_INVAL));
 }
 owner=1;put(cmd+22,1);
 for(unsigned len=0;len<28;len++)assert(host_session_combined_group(cmd,len,0)==(len==26?EFS_OK:EFS_ERR_INVAL));
 put(cmd+22,EFS_SESSION_BITS);assert(host_session_combined_group(cmd,26,0)==EFS_ERR_INVAL);
 put(cmd+22,UINT32_MAX);assert(host_session_combined_group(cmd,26,0)==EFS_ERR_INVAL);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-session-combined-group-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 flags=['-fsanitize=address,undefined'] if os.environ.get('EFS_TEST_SANITIZE')=='1' else []
 subprocess.run(['cc',*flags,'-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
a=s.index('static int host_apply_dispatch(',s.index('static int host_session_combined_group('));b=s.index('\n}',a)
assert s[a:b].index('host_session_combined_group(')<s[a:b].index('apply_session_cmd(')
a=s.index('static int apply_session_cmd(');b=s.index('\n}',a)
assert 'cmd[1] == EFS_MD_SESS_REGISTER_ESTABLISH ||' in s[a:b]
a=s.index('static int host_apply(void *app');b=s.index('\n}',a)
assert 'cmd[1] == EFS_MD_SESS_REGISTER_ESTABLISH ||' in s[a:b]
print('combined admission: exact length, owner/local group, shard limits, apply routing and audible logical verdicts PASS')
