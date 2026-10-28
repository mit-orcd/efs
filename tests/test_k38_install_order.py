#!/usr/bin/env python3
"""K38 loopback finding: an older snapshot's image must not erase newer body bytes.

Executes the production dcache_install_image (src/client/write.c). Order of
the Oct 25 loss (chunk 539): snapshot 641 copied, a write patched bytes
[10,20) into the body, snapshot 646 copied (dirty cleared), PUT 641 landed and
installed its image. The body must keep [10,20) (only snapshot 646 and the
body hold them); a STALE replay that copies the body before PUT 646 installs
would otherwise publish the chunk without them. K38_WRITE_SOURCE selects
another write.c (the pre-fix source) for the negative control.
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
src = Path(os.environ.get('K38_WRITE_SOURCE', root/'src/client/write.c')).read_text()


def fn(name):
    m = re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{', src, re.M)
    assert m, name
    return src[m.start():src.index('\n}', m.end())+2]


install = fn('dcache_install_image')
full_arg = ', full' if 'int unmerged_full' in install else ''
a = src.index('struct dcache_ent {')
code = r'''
#include "client_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define DCACHE_NR 32
#define DTRACE(e, ...) do { (void)(e); } while (0)
''' + src[a:src.index('\n};', a)+3] + r'''
static unsigned copied;
static void *copy_count(void *dst,const void *from,size_t n) {
 copied+=(unsigned)n;return memcpy(dst,from,n);
}
#undef memcpy
#define memcpy copy_count
''' + install + r'''
#undef memcpy
static void install_test(struct dcache_ent *e,uint8_t *img,uint32_t len,uint64_t seq,int full) {
 (void)full;dcache_install_image(e,img,len,seq''' + full_arg + r''');
}
''' + r'''
int main(void)
{
 static uint8_t body[64], img[64];
 struct dcache_ent e;
 memset(&e,0,sizeof(e));e.data=body;e.len=64;e.have_base=1;e.img_seq=628;
 memset(body,'o',64);memset(body,'A',10);           /* snapshot 641 copies 'A' */
 memcpy(img,body,64);                              /* PUT 641's image */
 memset(body+10,'B',10);e.nrange=1;e.roff[0]=0;e.rlen[0]=20;
 e.snap_seq=646;e.dirty=0;                         /* snapshot 646 has 'B', clears dirty */
 install_test(&e,img,64,641,0);              /* the older PUT lands */
 for(int i=10;i<20;i++)
  if(body[i]!='B'){fprintf(stderr,"K38 install order: PUT 641's image erased byte %d written after "
                   "its copy (only snapshot 646 holds it)\n",i);return 1;}
 assert(!memcmp(body,"AAAAAAAAAA",10)&&e.img_seq==641);
 /* the newer image installs as before; an older one after it is skipped */
 uint8_t img2[64];memcpy(img2,body,64);install_test(&e,img2,64,646,0);
 assert(e.img_seq==646);memset(img,'x',64);install_test(&e,img,64,641,0);
 assert(body[0]=='A'&&body[15]=='B'&&e.img_seq==646);
 /* the current snapshot's own image replaces the body outside dirty ranges */
 memset(img,'N',64);e.snap_seq=700;install_test(&e,img,64,700,0);
 assert(body[0]=='N'&&body[15]=='N'&&e.img_seq==700);
 /* dirty ranges stay overlaid, as before */
 e.dirty=1;e.roff[0]=30;e.rlen[0]=4;memset(body+30,'D',4);memset(img,'M',64);
 install_test(&e,img,64,701,0);assert(body[29]=='M'&&body[30]=='D'&&body[34]=='M');
 /* An unchanged full snapshot advances image identity without copying its
  * identical body back; this counter fails on the previous implementation. */
 e=(struct dcache_ent){.data=body,.len=64,.have_base=1,.snap_seq=800,.img_seq=701};
 memset(body,'U',64);memcpy(img,body,64);copied=0;
 install_test(&e,img,64,800,1);assert(!copied&&e.img_seq==800&&body[0]=='U');
 /* The same state without provenance MUST install its fetched/merged image. */
 e.snap_seq=801;memset(img,'V',64);copied=0;
 install_test(&e,img,64,801,0);assert(copied==64&&body[0]=='V'&&e.img_seq==801);
 /* Concurrent snapshot: preserve its later owned range, replace the rest. */
 e.snap_seq=803;e.nrange=1;e.roff[0]=10;e.rlen[0]=4;memset(body+10,'L',4);
 memset(img,'B',64);copied=0;install_test(&e,img,64,802,1);
 assert(copied==68&&body[9]=='B'&&body[10]=='L'&&body[14]=='B');
 /* Later unsnapshotted writes also force the overlay path. */
 e.dirty=1;e.snap_seq=804;memset(body+10,'D',4);memset(img,'C',64);copied=0;
 install_test(&e,img,64,804,1);assert(copied==68&&body[9]=='C'&&body[10]=='D');
 /* No identity, a short image, or a partial-body slot cannot use the shortcut. */
 e.dirty=0;e.nrange=0;e.snap_seq=805;memset(img,'S',64);copied=0;
 install_test(&e,img,32,805,1);assert(copied==32&&body[0]=='S');
 e.snap_seq=806;e.have_base=0;copied=0;install_test(&e,img,64,806,1);assert(copied==64);
 copied=0;install_test(&e,img,64,0,1);assert(copied==64);
 /* An obsolete completion never changes the body even when marked full. */
 memset(img,'X',64);copied=0;install_test(&e,img,64,805,1);assert(!copied&&body[0]=='S');
 puts("K38 install order: an older snapshot's image keeps newer body ranges; newer/skip/dirty behaviour and unchanged-full copy elision PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-k38-install-') as d:
    p = Path(d)/'test.c'; p.write_text(code)
    cmd = shlex.split(os.getenv('CC', 'cc'))+['-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
        '-pthread', '-I'+str(root/'include'), '-I'+str(root/'src/client')]
    cmd += shlex.split(os.environ.get('WB_RUNTIME_CFLAGS', ''))
    subprocess.run(cmd+[str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
