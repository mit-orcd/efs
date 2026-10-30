#!/usr/bin/env python3
"""Actual span reader: a recovered-node cooldown is not a lost object."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_DELTA_READ_SOURCE',root/'src/client/read.c')).read_text()
def fn(name):
 m=re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{',s,re.M);assert m,name
 return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include "client_internal.h"
#include "efs/erasure.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int mode,expired,get_calls,probes,decodes,decode_fail;
static unsigned down;
int efs_client_rpc_past_deadline(void){return expired;}
int efs_client_node_is_down(efs_node_id_t n){return !!(down&(1u<<n));}
int efs_client_node_reprobe(efs_node_id_t n){
 probes++;if(n==1){if(mode!=7)down&=~(1u<<n);return 1;}return 0;
}
int efs_client_get_fragment(efs_node_id_t n,efs_ino_t ino,uint32_t ci,uint32_t fi,
 uint32_t len,uint8_t *out,uint32_t *got,uint8_t sum[EFS_HASH_SIZE],uint64_t gen){
 assert(ino==123&&ci==3&&n==fi+1&&len==64&&gen==77);get_calls++;
 if(expired)return EFS_ERR_BUSY;
 if(mode==4)return EFS_ERR_NOT_FOUND;
 if(mode==1 || n==2 || efs_client_node_is_down(n))return EFS_ERR_NET;
 if(mode==2&&n==1)return EFS_ERR_CHECKSUM;
 *got=mode==3&&n==1?len-1:len;memset(out,0xa5,len);memset(sum,((mode==8||mode==9)&&n==1)?99:n,EFS_HASH_SIZE);return 0;
}
int efs_decode_chunk(uint8_t *const frags[3],size_t size,int a,int b,int missing,uint8_t *out,size_t len){
 decodes++;assert(size==128&&len==128&&a==0&&b==2&&missing==1);
 for(unsigned i=0;i<64;i++)assert(frags[a][i]==0xa5&&frags[b][i]==0xa5);
 if(decode_fail) return -1;
 memset(out,0xa5,len);
 return 0;
}
'''+fn('read_reprobe_down')+'\n'+fn('overlay_one_delta_ex')+'\n'+fn('overlay_one_delta')+r'''
static void reset(int m){mode=m;expired=get_calls=probes=decodes=decode_fail=0;down=0;}
int main(void){
 struct efs_chunk_delta d={.off=16,.len=48,.generation=77,.nodes={1,2,3}};
 for(unsigned i=0;i<3;i++)memset(d.checksums[i],i+1,EFS_HASH_SIZE);
 uint8_t out[128];
 reset(0);assert(!read_reprobe_down(d.nodes));down=(1u<<1)|(1u<<2);memset(out,0xcc,sizeof(out));
 assert(!overlay_one_delta(123,3,out,128,&d)&&get_calls==6&&probes==2&&decodes==1);
 for(unsigned i=0;i<sizeof(out);i++)assert(out[i]==(i>=16&&i<64?0xa5:0xcc));
 for(int m=1;m<=4;m++){
  reset(m);memset(out,0xcc,sizeof(out));int rc=overlay_one_delta(123,3,out,128,&d);
  assert(rc==((m==2||m==3)?EFS_ERR_CHECKSUM:EFS_ERR_DECODE)&&get_calls==3&&!probes&&!decodes);
  for(unsigned i=0;i<sizeof(out);i++)assert(out[i]==0xcc);
 }
 reset(8);assert(overlay_one_delta(123,3,out,128,&d)==EFS_ERR_CHECKSUM&&!probes&&!decodes);
 reset(9);down=(1u<<1)|(1u<<2);assert(overlay_one_delta(123,3,out,128,&d)==EFS_ERR_CHECKSUM&&get_calls==6&&probes==2&&!decodes);
 reset(5);assert(!overlay_one_delta(123,3,out,128,&d)&&get_calls==3&&!probes&&decodes==1);
 reset(7);down=(1u<<1)|(1u<<2);assert(overlay_one_delta(123,3,out,128,&d)==EFS_ERR_DECODE&&get_calls==6&&probes==2&&!decodes);
 reset(0);expired=1;down=(1u<<1)|(1u<<2);int transient=0;
 assert(overlay_one_delta_ex(123,3,out,128,&d,&transient)==EFS_ERR_DECODE&&transient&&!probes&&!decodes);
 reset(5);decode_fail=1;assert(overlay_one_delta(123,3,out,128,&d)==EFS_ERR_DECODE&&decodes==1&&!probes);
 reset(0);d.len=129;assert(overlay_one_delta(123,3,out,128,&d)==EFS_ERR_INVAL&&!get_calls&&!probes&&!decodes);
 puts("span reader: recovered cooldown re-probed once, exact recorded identity/hash and span bounds, genuine outage/corruption/missing object/expired budget still fail PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-delta-reprobe-') as td:
 p=Path(td)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
