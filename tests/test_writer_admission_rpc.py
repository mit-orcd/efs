#!/usr/bin/env python3
"""Warm admission never consults inode authority; errors never trigger adoption."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/inode_rpc.c').read_text();start=s.index('int efs_client_rpc_writer_admission_view(')
function=s[start:s.index('\n}',start)+2]
source=r'''
#include "efs/protocol.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int efs_client_rpc_past_deadline(void) { return 0; }
static unsigned reads,boots; static int read_rc,boot_rc,bad;
int efs_chunk_size_valid(uint32_t cs) { return cs==EFS_MIN_CHUNK_SIZE; }
int efs_client_rpc_lane_writer_view(efs_ino_t ino,uint64_t gen,uint32_t ci,uint32_t cs,
                                    struct efs_msg_lane_writer_view_reply *out) {
    ++reads; *out=(struct efs_msg_lane_writer_view_reply){0};
    out->view.ino=ino;out->view.generation=gen+bad;out->view.chunk_index=ci;out->chunk_size=cs;
    return read_rc;
}
int efs_client_rpc_lane_bootstrap(efs_export_id_t ex,efs_ino_t ino,uint64_t gen,uint32_t ci,
                                  uint32_t cs,struct efs_msg_lane_writer_view_reply *out) {
    assert(ex==19);++boots;
    out->view.ino=ino;out->view.generation=gen+bad;out->view.chunk_index=ci;out->chunk_size=cs;
    return boot_rc;
}
''' + function + r'''
int main(void) {
    struct efs_msg_lane_writer_view_reply out,saved;memset(&saved,0x5a,sizeof(saved));
    out=saved;
    assert(efs_client_rpc_writer_admission_view(19,123,456,17,EFS_MIN_CHUNK_SIZE,&out)==EFS_OK && reads==1 && !boots);
    read_rc=EFS_ERR_NOT_FOUND;
    assert(efs_client_rpc_writer_admission_view(19,123,456,17,EFS_MIN_CHUNK_SIZE,&out)==EFS_OK && boots==1);
    int errors[]={EFS_ERR_BUSY,EFS_ERR_STALE,EFS_ERR_NET,EFS_ERR_PROTO};
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);++i) {
        out=saved;read_rc=errors[i];
        assert(efs_client_rpc_writer_admission_view(19,123,456,17,EFS_MIN_CHUNK_SIZE,&out)==errors[i] && boots==1 && !memcmp(&out,&saved,sizeof(out)));
    }
    read_rc=EFS_ERR_NOT_FOUND;boot_rc=EFS_ERR_BUSY;out=saved;
    assert(efs_client_rpc_writer_admission_view(19,123,456,17,EFS_MIN_CHUNK_SIZE,&out)==EFS_ERR_BUSY && !memcmp(&out,&saved,sizeof(out)));
    read_rc=0;bad=1;
    assert(efs_client_rpc_writer_admission_view(19,123,456,17,EFS_MIN_CHUNK_SIZE,&out)==EFS_ERR_PROTO && !memcmp(&out,&saved,sizeof(out)));
    unsigned calls=reads;
    assert(efs_client_rpc_writer_admission_view(19,123,0,17,EFS_MIN_CHUNK_SIZE,&out)==EFS_ERR_INVAL && reads==calls);
    puts("writer admission: warm lane-only path, cold absence only, no fallback on errors and immutable failure output PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-admission-rpc-') as directory:
    path=Path(directory)/'test.c';path.write_text(source)
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include')]
    cmd+=shlex.split(os.environ.get('WRITER_RPC_TEST_CFLAGS',''))
    subprocess.run(cmd+[str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
