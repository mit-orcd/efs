#!/usr/bin/env python3
"""Diagnostic observer preserves mmap contents/lifetime and captures ownership."""
import ctypes
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
if platform.system() != 'Linux' or platform.machine() != 'x86_64':
    print('NOTRUN: allocator observer requires assigned x86_64 Linux/glibc host')
    raise SystemExit(0)
try:
    ctypes.CDLL(None).mallinfo2
except AttributeError:
    print('NOTRUN: allocator observer requires glibc mallinfo2')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='efs-alloc-observer-test-') as directory:
    d = Path(directory)
    source = d/'test.c'
    source.write_text(r'''
#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
int main(void) {
    size_t n = 1024*1024;
    volatile unsigned char *p = mmap(0,n,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if ((void *)p == MAP_FAILED) return 2;
    for(size_t i=0;i<n;i++) p[i]=(unsigned char)i;
    errno=0;
    if(mmap(0,0,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)!=MAP_FAILED || errno!=EINVAL) return 5;
    errno=0;
    if(munmap((void *)p,0)!=-1 || errno!=EINVAL) return 6;
    sleep(3);
    for(size_t i=0;i<n;i++) if(p[i]!=(unsigned char)i) return 3;
    printf("mapped=%p\n",(void *)p);
    return munmap((void *)p,n) ? 4 : 0;
}
''')
    exe = d/'efs-fuse';observer = d/'observer.so'
    subprocess.run(['cc','-O2','-pthread',str(source),'-o',str(exe)],check=True)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC','-pthread',
                    str(root/'scripts/task_i_alloc_probe.c'),'-o',str(observer)],check=True)
    (d/'load-start').write_text(str(time.time()-3601)+'\n')
    env = dict(os.environ,LD_PRELOAD=str(observer),TASK_I_ALLOC_DIR=str(d))
    result = subprocess.run([str(exe)],env=env,text=True,capture_output=True,timeout=10,check=True)
    address = result.stdout.strip().split('mapped=')[1]
    events = next(d.glob('*-mappings.log')).read_text().splitlines()
    assert any(s.startswith('mmap '+address+' 1048576 ') for s in events),events
    assert any(s.startswith('munmap '+address+' 1048576 ') for s in events),events
    for minute in (20,40,60):
        payload=json.loads(next(d.glob(f'*-{minute}m.json')).read_text())
        assert payload['elapsed_s']>=3600 and payload['arena']>0
        ET.parse(next(d.glob(f'*-{minute}m.xml')))
        text=next(d.glob(f'*-{minute}m.smaps')).read_text()
        assert 'Rss:' in text and 'VmFlags:' in text
    assert next(d.glob('*-maps.txt')).read_text()
print('allocator observer: exact mmap payload/unmap, mapping identity and all six allocation/smaps captures PASS')
