#!/usr/bin/env python3
"""RDMA dispatch must align typed requests without dropping assembly ownership."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
handler = (root / 'src/server/handler.c').read_text()
start = handler.index('            type = frame[4];')
block = handler[start:handler.index('        } else {', start)]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
struct request { uint64_t export_id, ino, flags; };
static void check(unsigned length) {
    unsigned char *frame = malloc(length+5); assert(frame);
    struct request expected = {1, 0x123456789abcULL, 7};
    memcpy(frame+5, &expected, sizeof(expected)); frame[4]=47;
    void *payload=0; uint32_t payload_len=0,nlen=length+1;
    uint8_t type=0;int rdma_frame=0;
''' + block + r'''
    assert(type==47 && rdma_frame && payload_len==length);
    struct request *request=payload;
    assert(request->export_id==1 && request->ino==expected.ino && request->flags==7);
    assert(payload==frame); /* same allocation and assembly queue ownership */
    free(frame);
}
int main(void) {check(sizeof(struct request));check(65536);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-rdma-alignment-') as directory:
    source = Path(directory) / 'dispatch.c'
    binary = source.with_suffix('')
    source.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
        '-fno-sanitize-recover=all', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('RDMA handler: naturally aligned typed payload, same charged allocation PASS')
