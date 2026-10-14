#!/usr/bin/env python3
"""Execute the production REPORT size-selection block; zero is authoritative.

Extracts the pure selection block (scan the report's size records for the
chunk's inode) from src/server/raft_host.c and compiles it standalone.
Contract: an explicit zero size record means EOF 0 (never chunk-rounded),
recorded sizes pass through unmodified, and a MISSING record is reported
via have_size==0 — the production loop then refuses the batch
(pack-nosize), which the live oracle tests/live/report_eof_contract.py
--expect refused covers end to end."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/server/raft_host.c').read_text()
start = source.index('            sz = 0;', source.index('void server_raft_host_report('))
end = source.index('            /* Zero is an explicit', start)
block = source[start:end]
code = r'''
#include "efs/protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint64_t selected(uint32_t ci,const struct efs_ino_size_rec *irecs,uint32_t ino_count,int *have) {
    struct efs_chunk_rec recs[1]={{.ino=42,.chunk_index=ci}};
    uint32_t i=0,j,clen=0;uint8_t lg=0;uint64_t sz=0;
    int have_size=0;
''' + block + r'''
    (void)clen;(void)lg;
    *have=have_size;
    return sz;
}
int main(void) {
    int have;
    struct efs_ino_size_rec records[2]={{.ino=7,.size=123},{.ino=42,.size=0}};
    assert(selected(64,records,2,&have)==0 && have);
    records[1].size=8388609;assert(selected(64,records,2,&have)==8388609 && have);
    records[1].size=1048577;assert(selected(8,records,2,&have)==1048577 && have);
    /* A missing record is signalled, never chunk-rounded; the caller refuses. */
    assert(selected(64,records,1,&have)==0 && !have);
    assert(selected(64,NULL,0,&have)==0 && !have);
    puts("REPORT explicit zero/non-rounded logical EOF and missing-record refusal signal PASS");
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
