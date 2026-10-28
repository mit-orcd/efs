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


a = src.index('struct dcache_ent {')
code = r'''
#include "client_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define DCACHE_NR 32
#define DTRACE(e, ...) do { (void)(e); } while (0)
''' + src[a:src.index('\n};', a)+3] + '\n' + fn('dcache_install_image') + r'''
int main(void)
{
 static uint8_t body[64], img[64];
 struct dcache_ent e;
 memset(&e,0,sizeof(e));e.data=body;e.len=64;e.have_base=1;e.img_seq=628;
 memset(body,'o',64);memset(body,'A',10);           /* snapshot 641 copies 'A' */
 memcpy(img,body,64);                              /* PUT 641's image */
 memset(body+10,'B',10);e.nrange=1;e.roff[0]=0;e.rlen[0]=20;
 e.snap_seq=646;e.dirty=0;                         /* snapshot 646 has 'B', clears dirty */
 dcache_install_image(&e,img,64,641);              /* the older PUT lands */
 for(int i=10;i<20;i++)
  if(body[i]!='B'){fprintf(stderr,"K38 install order: PUT 641's image erased byte %d written after "
                   "its copy (only snapshot 646 holds it)\n",i);return 1;}
 assert(!memcmp(body,"AAAAAAAAAA",10)&&e.img_seq==641);
 /* the newer image installs as before; an older one after it is skipped */
 uint8_t img2[64];memcpy(img2,body,64);dcache_install_image(&e,img2,64,646);
 assert(e.img_seq==646);memset(img,'x',64);dcache_install_image(&e,img,64,641);
 assert(body[0]=='A'&&body[15]=='B'&&e.img_seq==646);
 /* the current snapshot's own image replaces the body outside dirty ranges */
 memset(img,'N',64);e.snap_seq=700;dcache_install_image(&e,img,64,700);
 assert(body[0]=='N'&&body[15]=='N'&&e.img_seq==700);
 /* dirty ranges stay overlaid, as before */
 e.dirty=1;e.roff[0]=30;e.rlen[0]=4;memset(body+30,'D',4);memset(img,'M',64);
 dcache_install_image(&e,img,64,701);assert(body[29]=='M'&&body[30]=='D'&&body[34]=='M');
 puts("K38 install order: an older snapshot's image keeps newer body ranges; newer/skip/dirty behaviour kept PASS");
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
