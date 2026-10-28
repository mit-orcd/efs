#!/usr/bin/env python3
"""Real glibc arena budget under concurrent FUSE-like scratch allocations.

The negative control omits the binary startup policy and must exceed four
arenas. Payload checks and active-worker arena counting test behavior, not
source spelling or an assumed allocator environment setting.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
s = (root/'src/client/efs_fuse.c').read_text()
m = re.search(r'^static int fuse_allocator_init\(void\)\n\{', s, re.M)
assert m
policy = s[m.start():s.index('\n}',m.end())+2]
source = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef __GLIBC__
#include <malloc.h>
'''+policy+r'''
#define N 24
static pthread_barrier_t ready,release;
static void *worker(void *arg) {
    unsigned char *p[8];int id=(int)(long)arg;
    for(int i=0;i<8;i++) {
        p[i]=malloc(65536);if(!p[i])abort();memset(p[i],id,65536);
    }
    pthread_barrier_wait(&ready);pthread_barrier_wait(&release);
    for(int i=0;i<8;i++) {
        for(int j=0;j<65536;j++)if(p[i][j]!=(unsigned char)id)abort();
        free(p[i]);
    }
    return NULL;
}
int main(int argc,char **argv) {
    (void)argv;
    if(argc==1 && fuse_allocator_init())return 2;
    pthread_t t[N];pthread_barrier_init(&ready,0,N+1);pthread_barrier_init(&release,0,N+1);
    for(long i=0;i<N;i++)if(pthread_create(&t[i],0,worker,(void *)i))return 3;
    pthread_barrier_wait(&ready);
    FILE *f=tmpfile();if(!f || malloc_info(0,f))return 4;
    rewind(f);char line[512];int heaps=0;
    while(fgets(line,sizeof(line),f))if(strstr(line,"<heap nr="))heaps++;
    fclose(f);pthread_barrier_wait(&release);
    for(int i=0;i<N;i++)pthread_join(t[i],0);
    printf("arenas=%d active_workers=%d payloads=exact\n",heaps,N);
    return heaps<=4?0:1;
}
#else
int main(void) {puts("client allocator: NOTRUN non-glibc allocator has no glibc arena policy");return 3;}
#endif
'''
with tempfile.TemporaryDirectory(prefix='efs-client-allocator-',dir=os.environ.get('TMPDIR')) as tmp:
    p=Path(tmp);(p/'case.c').write_text(source)
    subprocess.run([os.environ.get('CC','cc'),'-O2','-pthread',str(p/'case.c'),'-o',str(p/'case')],check=True)
    env=dict(os.environ,MALLOC_ARENA_MAX='128')
    positive=subprocess.run([str(p/'case')],env=env,text=True,capture_output=True,timeout=30)
    if positive.returncode==3 and 'NOTRUN non-glibc' in positive.stdout:
        print(positive.stdout.strip());raise SystemExit(0)
    assert positive.returncode==0,positive.stdout+positive.stderr
    negative=subprocess.run([str(p/'case'),'unfixed-control'],env=env,text=True,capture_output=True,timeout=30)
    assert negative.returncode==1,negative.stdout+negative.stderr
    print('fixed:',positive.stdout.strip())
    print('unfixed negative control:',negative.stdout.strip())
print('client allocator fixed budget independent of launcher/environment: PASS')
