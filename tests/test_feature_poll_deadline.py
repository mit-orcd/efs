#!/usr/bin/env python3
"""Execute the actual advisory feature-poll wrapper's shared deadline scope."""
from pathlib import Path
import shlex,subprocess,tempfile,os
root=Path(__file__).resolve().parents[1];s=(root/'src/client/efs_fuse.c').read_text()
a=s.index('static int features_poll(uint32_t *out)');b=s.index('\n}',a)+2
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint64_t now=10000,deadline,observed;
static int result;
static uint64_t stats_now_ms(void){return now;}
static uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
static void efs_client_rpc_set_deadline_ms(uint64_t v){deadline=v;}
static int features_poll_wire(uint32_t*out){observed=deadline;*out=3;return result;}
'''+s[a:b]+r'''
int main(void){
 uint32_t mask=0;
 for(result=0;result<=1;result++){
  deadline=0;assert(features_poll(&mask)==result && observed==11000 && deadline==0 && mask==3);
  deadline=10500;assert(features_poll(&mask)==result && observed==10500 && deadline==10500);
  deadline=12000;assert(features_poll(&mask)==result && observed==11000 && deadline==12000);
  deadline=9999;assert(features_poll(&mask)==result && observed==9999 && deadline==9999);
 }
 puts("feature discovery: bounded refresh, tighter/expired caller deadline retained, scope restored on success/failure PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-feature-deadline-') as td:
 p=Path(td)/'t.c';p.write_text(code);subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror',str(p),'-o',str(p.with_suffix(''))],check=True);subprocess.run([str(p.with_suffix(''))],check=True)
