#!/usr/bin/env python3
"""Execute the production REPORT size-selection block; zero is authoritative."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/server/raft_host.c').read_text()
start = source.index('            sz = 0;', source.index('void server_raft_host_report('))
end = source.index('            for (k = 0; k < nic;', start)
block = source[start:end]
code = r'''
#include "efs/protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint64_t selected(uint32_t ci,const struct efs_ino_size_rec *irecs,uint32_t ino_count) {
    struct efs_chunk_rec recs[1]={{.ino=42,.chunk_index=ci}};
    uint32_t i=0,j,clen=0;uint8_t lg=0;uint64_t sz=0;
    int have_size=0;
''' + block + r'''
    (void)clen;(void)lg;(void)have_size;
    return sz;
}
int main(void) {
    struct efs_ino_size_rec records[2]={{.ino=7,.size=123},{.ino=42,.size=0}};
    assert(selected(64,records,2)==0);
    records[1].size=8388609;assert(selected(64,records,2)==8388609);
    records[1].size=1048577;assert(selected(8,records,2)==1048577);
    /* Missing-record behavior is retained, not claimed fixed here. */
    assert(selected(64,records,1)==8519680);
    assert(selected(64,NULL,0)==8519680);
    puts("REPORT explicit zero/non-rounded logical EOF and separate absent record PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-zero-') as work:
    path = Path(work) / 'test.c'
    path.write_text(code)
    binary = path.with_suffix('')
    cc = shlex.split(os.environ.get('CC', 'cc'))
    subprocess.run(cc + ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                         '-I' + str(root / 'include'), str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
