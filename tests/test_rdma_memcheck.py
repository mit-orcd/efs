#!/usr/bin/env python3
"""Verify DMA annotation boundaries and detection of undefined outgoing bytes."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = r'''
#include "src/common/rdma_memcheck.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static void bits(unsigned char *p, unsigned prefix) {
    unsigned char v[32];
    assert(VALGRIND_GET_VBITS(p, v, sizeof(v)) == 1);
    for (unsigned i=0; i<32; i++) assert(v[i] == (i<prefix ? 0 : 255));
}
int main(int argc, char **argv) {
    (void)argv;
    unsigned char *p=malloc(32); assert(p);
    memset(p, 1, 32);
    rdma_memcheck_post_recv(p, 32); bits(p, 0);
    rdma_memcheck_recv_complete(p, 32, 8); bits(p, 8);
    rdma_memcheck_send(p, 8);
    rdma_memcheck_post_recv(p, 32); bits(p, 0);
    rdma_memcheck_recv_complete(p, 32, 33); bits(p, 0);
    rdma_memcheck_recv_complete(p, 32, 4); bits(p, 4);
    if (argc>1) rdma_memcheck_send(p+4, 1);
    free(p); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-rdma-memcheck-') as directory:
    source = Path(directory)/'test.c'
    binary = Path(directory)/'test'
    source.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=c99', '-DEFS_MEMCHECK=1', '-I'+str(root),
                    str(source), '-o', str(binary)], check=True)
    for negative in (False, True):
        result = subprocess.run(['valgrind', '--error-exitcode=99',
                                 '--leak-check=full', str(binary)] +
                                (['undefined-send'] if negative else []),
                                capture_output=True, text=True)
        assert result.returncode == (99 if negative else 0), result.stderr
        if negative:
            assert 'client check request' in result.stderr, result.stderr
print('RDMA Memcheck: bounded completion, repost/tail poisoning, undefined send detection PASS')
