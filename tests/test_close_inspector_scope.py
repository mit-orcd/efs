#!/usr/bin/env python3
"""Execute the real offline inspector callback with owned and unrelated keys."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=Path(os.environ.get('EFS_CLOSE_STEP_SOURCE',str(root/'tests/live/close_step.c'))).read_text();a=s.index('static unsigned markers');b=s.index('int main(',a)
code=r'''
#include "efs/common.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
'''+s[a:b]+r'''
static void item(unsigned kind,unsigned ino,unsigned len) {
 uint8_t key[64]={0};key[2]=kind;key[10]=ino;efs_ino_t owned=42;
 assert(!count(&owned,key,len,NULL,0));
}
int main(void) {
 item(EFS_KV_KIND_CLOSE_STEP,42,11);item(EFS_KV_KIND_CLOSE_STEP,7,11);
 item(EFS_KV_KIND_APPEND_RSV,42,31);item(EFS_KV_KIND_APPEND_RSV,7,31);
 item(EFS_KV_KIND_APPEND_RECEIPT,42,31);item(EFS_KV_KIND_APPEND_RECEIPT,7,31);
 assert(markers==1&&reservations==1&&receipts==1);
 for(unsigned len=0;len<11;len++)item(EFS_KV_KIND_CLOSE_STEP,42,len);
 assert(markers==1&&reservations==1&&receipts==1);
 puts("offline close inspector: owned markers/reservations/receipts only; unrelated and short keys do not contaminate the verdict PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-close-inspector-') as td:
 p=Path(td)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
