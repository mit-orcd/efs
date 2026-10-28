#!/usr/bin/env python3
"""Actual sparse-metadata policy avoids THP amplification without losing bytes."""
from pathlib import Path
import platform
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
assert 'efs_sparse_metadata_pages(&g_dcache, sizeof(g_dcache))' in (root/'src/client/write.c').read_text()
if platform.system()!='Linux':
    print('NOTRUN: sparse metadata residency test needs Linux smaps')
    raise SystemExit(0)
code=r'''
#define _GNU_SOURCE
#include "sparse_metadata.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HUGE (2ul*1024*1024)
#define SIZE (48ul*1024*1024)
static void run(int fixed) {
    unsigned char *all=mmap(0,SIZE+HUGE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(all!=MAP_FAILED);
    uintptr_t aligned=((uintptr_t)all+HUGE-1)&~(HUGE-1);
    unsigned char *p=(unsigned char *)aligned;
    size_t prefix=(size_t)(p-all),tail=HUGE-prefix;
    if(prefix)assert(!munmap(all,prefix));
    if(tail)assert(!munmap(p+SIZE,tail));
    assert(!madvise(p,SIZE,MADV_HUGEPAGE)); /* deterministic equivalent of host always */
    if(fixed) assert(!efs_sparse_metadata_pages(p+17,SIZE-34)); /* unaligned metadata */
    for(size_t i=0;i<SIZE;i+=HUGE)p[i]=(unsigned char)(i/HUGE+1);
    for(size_t i=0;i<SIZE;i+=HUGE)assert(p[i]==(unsigned char)(i/HUGE+1));
    char line[512];FILE *f=fopen("/proc/self/smaps","r");assert(f);
    unsigned long a,b,rss=0,huge=0;int in=0,nohuge=0;
    while(fgets(line,sizeof(line),f)) {
        if(sscanf(line,"%lx-%lx",&a,&b)==2)in=(uintptr_t)p>=a&&(uintptr_t)p<b;
        else if(in) {
            unsigned long n;
            if(sscanf(line,"Rss: %lu kB",&n)==1)rss=n;
            if(sscanf(line,"AnonHugePages: %lu kB",&n)==1)huge=n;
            if(!strncmp(line,"VmFlags:",8)&&strstr(line," nh"))nohuge=1;
        }
    }
    fclose(f);
    if(fixed)assert(nohuge && !huge && rss<=512);
    printf("%s rss_kib=%lu thp_kib=%lu payloads=exact\n",fixed?"fixed":"old-negative",rss,huge);
    if(!fixed && !huge)puts("NOTRUN: host did not materialize THP negative; fixed small-page proof still ran");
    else if(!fixed)assert(rss>=2048); /* even one THP amplifies sparse base pages */
    assert(!munmap(p,SIZE));
}
int main(void){run(1);run(0);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-sparse-pages-') as directory:
    d=Path(directory);(d/'test.c').write_text(code)
    subprocess.run(['cc','-std=c99','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'src/client'),str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True,timeout=30)
print('sparse metadata policy: original-capacity sparse payloads, unaligned range, no huge pages PASS')
