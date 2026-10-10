#!/usr/bin/env python3
"""Actual post-leadership-check forwarding tail must not mix policy/log indices."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'src/server/raft_host.c').read_text()
begin = source.index("    /* APPEND's reserved offset", source.index('static int host_propose_impl('))
end = source.index('\n}\n', begin)
code = '''
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdio.h>
struct efs_raft_host {int unused;};
static unsigned calls;
static int host_remote_cmd(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,
 uint32_t clen,struct efs_msg_raft_mkfs_reply *reply,int lid)
{
 (void)h;assert(group==0 && cmd && clen==1 && lid==2);calls++;
 *reply=(struct efs_msg_raft_mkfs_reply){.index=777,.term=9,.leader_hint=2};return EFS_OK;
}
static int tail(unsigned tag,uint64_t *idx,uint64_t *term,int *leader_hint)
{
 struct efs_raft_host host={0},*h=&host;uint8_t bytes[1]={tag},*cmd=bytes;
 uint32_t clen=1;uint8_t group=0;int rc,lid=2;struct efs_msg_raft_mkfs_reply rep;
''' + source[begin:end] + '''
}
int main(void)
{
 uint64_t index=0,term=0;int hint=-1;
 assert(tail(EFS_MD_CMD_QUERY_POLICY,&index,&term,&hint)==EFS_ERR_NOT_PRIMARY);
 assert(!calls && !index && !term);
 assert(tail(EFS_MD_CMD_APPEND_RSV,&index,&term,&hint)==EFS_ERR_NOT_PRIMARY && !calls);
 assert(!tail(EFS_MD_CMD_CREATE,&index,&term,&hint));
 assert(calls==1 && index==777 && term==9 && hint==2);
 puts("policy leadership race: explicit redirect; no policy-version/apply-ring mix; ordinary forwarding retained PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-policy-forward-') as directory:
    path = Path(directory) / 'test.c'
    path.write_text(code)
    executable = path.with_suffix('')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra',
                   '-Werror', '-I' + str(root / 'include'), str(path), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
