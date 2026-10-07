#!/usr/bin/env python3
"""Exercise production parallel GET verifier with corrupt wire replies."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
read = (root/'src/client/read.c').read_text()
def function(name):
    match=re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{',read,re.M)
    assert match,name
    return read[match.start():read.index('\n}',match.end())+2]
start=read.index('struct frag_get_job {')
job=read[start:read.index('\n};',start)+3]
source=r'''
#include "efs/protocol.h"
#include "efs/checksum.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static uint8_t bytes[65536], sum[EFS_HASH_SIZE];
static pthread_once_t read_verify_once=PTHREAD_ONCE_INIT;
static int read_verify;
int efs_conn_recv_msg_into(struct efs_conn *conn,uint8_t *type,uint8_t *status,
                          void *header,uint32_t hn,void *body,uint32_t bn) {
    (void)conn; assert(hn==EFS_HASH_SIZE && bn==sizeof(bytes));
    *type=EFS_MSG_GET_CHUNK_REPLY; *status=EFS_GET_CHUNK_OK;
    memcpy(header,sum,hn);memcpy(body,bytes,bn);return 0;
}
''' + job + '\n' + '\n'.join(function(name) for name in ('read_verify_init','fragment_reply_verified','get_one_reply')) + r'''
int main(void) {
    uint8_t out[65536];
    struct frag_get_job job={.frag_len=sizeof(out),.out=out};
    memset(bytes,0,sizeof(bytes));efs_hash(bytes,sizeof(bytes),sum);
    assert(get_one_reply(NULL,&job)==0 && job.rc==EFS_OK);
    bytes[1024]=1;job.len=0;
    assert(get_one_reply(NULL,&job)==0 && job.rc==EFS_ERR_CHECKSUM && job.len==0);
    memset(bytes,'x',sizeof(bytes));efs_hash(bytes,sizeof(bytes),sum);
    assert(get_one_reply(NULL,&job)==0 && job.rc==EFS_OK);
    bytes[32768]='y';job.len=0;
    assert(get_one_reply(NULL,&job)==0 && job.rc==EFS_ERR_CHECKSUM && job.len==0);
    uint8_t expected[EFS_HASH_SIZE];
    efs_hash(bytes,sizeof(bytes),sum);memcpy(expected,sum,sizeof(expected));
    job.expected_sum=expected;
    assert(get_one_reply(NULL,&job)==0 && job.rc==EFS_OK);
    memset(bytes,'z',sizeof(bytes));efs_hash(bytes,sizeof(bytes),sum);job.len=0;
    assert(get_one_reply(NULL,&job)==0 && job.rc==EFS_ERR_CHECKSUM && job.len==0);
    puts("parallel GET: valid zero/nonzero and corrupt zero/nonzero replies PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-verify-') as directory:
    path=Path(directory)/'test.c';path.write_text(source)
    binary=path.with_suffix('')
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'include'),str(path),str(root/'libefs.a'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],env=dict(os.environ,EFS_READ_VERIFY='1'),check=True)
