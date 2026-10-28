#!/usr/bin/env python3
"""Default RoCE address selection avoids the observed RXE link-local trap."""
import os,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'src/common/rdma.c').read_text();a=s.index('static int gid_link_local(');b=s.index('static int dev_open(',a);helpers=s[a:b]
pre=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
union ibv_gid {uint8_t raw[16];};
struct ibv_context {int unused;};
static union ibv_gid table[4];static int query_fail=-1;
static int ibv_query_gid(struct ibv_context *ctx,int port,int index,union ibv_gid *out){
 (void)ctx;assert(port==1 && index>=0 && index<4);if(index==query_fail)return -1;*out=table[index];return 0;}
'''
post=r'''
int main(void){struct ibv_context ctx={0};union ibv_gid out;
 table[0].raw[0]=0xfe;table[0].raw[1]=0x80;table[0].raw[15]=1;
 table[1].raw[10]=0xff;table[1].raw[11]=0xff;table[1].raw[12]=10;table[1].raw[15]=206;
 if(gid_select(&ctx,1,4,-1,&out)!=1){fputs("default chose link-local instead of available IPv4 GID\n",stderr);return 10;}
 assert(!memcmp(&out,&table[1],sizeof(out)));
 assert(gid_select(&ctx,1,4,0,&out)==0 && out.raw[0]==0xfe);
 assert(gid_select(&ctx,1,4,1,&out)==1);
 query_fail=1;assert(gid_select(&ctx,1,4,1,&out)==-1);query_fail=-1;
 memset(&table[1],0,sizeof(table[1]));assert(gid_select(&ctx,1,4,-1,&out)==0);
 table[2].raw[0]=0x20;table[2].raw[1]=1;table[2].raw[15]=1;
 assert(gid_select(&ctx,1,4,-1,&out)==2 && out.raw[0]==0x20);
 table[0]=table[2];table[1].raw[10]=table[1].raw[11]=0xff;table[1].raw[12]=10;table[1].raw[15]=206;
 assert(gid_select(&ctx,1,4,-1,&out)==0); /* no forced IPv4 preference over global IPv6 */
 memset(table,0,sizeof(table));assert(gid_select(&ctx,1,4,-1,&out)==-1);
 assert(gid_select(&ctx,1,0,-1,&out)==-1 && gid_select(&ctx,1,4,4,&out)==-1 && gid_select(&ctx,1,4,-2,&out)==-1);
 puts("RoCE GID: global over link-local, global IPv6 preserved, explicit index exact, absent/query errors refuse PASS");return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-rdma-gid-',dir=os.environ.get('TMPDIR')) as d:
 p=Path(d)
 for name,body,expected in [('fixed',helpers,0),('first-nonzero-control',helpers.replace('requested >= 0 || !gid_link_local(&candidate)','requested >= -1 || !gid_link_local(&candidate)'),10)]:
  source=p/(name+'.c');source.write_text(pre+body+post)
  subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',str(source),'-o',str(p/name)],check=True)
  r=subprocess.run([str(p/name)],capture_output=True,text=True,timeout=5)
  assert r.returncode==expected,r.stdout+r.stderr
  print(name+': '+(r.stdout+r.stderr).strip())
