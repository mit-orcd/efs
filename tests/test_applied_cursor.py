#!/usr/bin/env python3
"""Run actual restart cursor decoding with KV and legacy checkpoint fault inputs."""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
def function(name):
 a=s.index('static '+name);b=s.index('\n}',a)+2
 return s[a:b]
code=r'''
#include "efs/kv.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
struct host_group {uint8_t group;};
struct efs_raft_host {struct efs_kv *kv;struct host_group g[2];};
static uint8_t saved[16],legacy[16];static uint32_t saved_len;static int kv_rc,legacy_len;
static uint64_t rd64be(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=v<<8|p[i];return v;}
static void wr64be(uint8_t *p,uint64_t v){for(int i=7;i>=0;i--){p[i]=v;v>>=8;}}
static void applied_path(struct efs_raft_host *h,uint8_t g,char *p,size_t n){(void)h;(void)g;snprintf(p,n,"fixture");}
static int fixture_get(struct efs_kv *k,const uint8_t *key,uint32_t kl,uint8_t *v,uint32_t *vl){
 (void)k;assert(kl==7 && key[0]==255 && key[1]==255 && key[6]==2);
 if(kv_rc)return kv_rc;
 if(*vl<saved_len)return EFS_ERR_INVAL;
 memcpy(v,saved,saved_len);*vl=saved_len;return EFS_OK;
}
static int fixture_open(const char *p,int flags){(void)p;assert(flags==O_RDONLY);return legacy_len<0?-1:4;}
static int fixture_read(int fd,void *b,size_t n){assert(fd==4 && n==16);memcpy(b,legacy,legacy_len);return legacy_len;}
static int fixture_close(int fd){assert(fd==4);return 0;}
#define efs_kv_get fixture_get
#define open fixture_open
#define read fixture_read
#define close fixture_close
''' + function('void host_kv_cursor(')+'\n'+function('int load_applied(')+r'''
static void proof(uint64_t i){uint8_t key[7];host_kv_cursor(2,i,key,saved);saved_len=16;kv_rc=EFS_OK;}
static void hint(uint64_t i,int length){wr64be(legacy,i);wr64be(legacy+8,~i);legacy_len=length;}
int main(void){
 struct efs_raft_host h={0};h.g[0].group=2;uint64_t i;
 proof(100);legacy_len=-1;assert(load_applied(&h,0,&i)==EFS_OK && i==100);
 hint(90,16);assert(load_applied(&h,0,&i)==EFS_OK && i==100);
 hint(110,16);assert(load_applied(&h,0,&i)==EFS_OK && i==110);
 hint(120,8);assert(load_applied(&h,0,&i)==EFS_OK && i==120);
 hint(200,16);legacy[15]^=1;assert(load_applied(&h,0,&i)==EFS_OK && i==100);
 legacy_len=7;assert(load_applied(&h,0,&i)==EFS_OK && i==100);
 kv_rc=EFS_ERR_NOT_FOUND;hint(120,16);assert(load_applied(&h,0,&i)==EFS_OK && i==120);
 legacy_len=-1;assert(load_applied(&h,0,&i)==EFS_OK && i==0);
 proof(100);saved[15]^=1;assert(load_applied(&h,0,&i)==EFS_ERR_PROTO);
 proof(100);saved_len=8;assert(load_applied(&h,0,&i)==EFS_ERR_PROTO);
 int failures[]={EFS_ERR_IO,EFS_ERR_NOMEM,EFS_ERR_BUSY};
 for(unsigned j=0;j<sizeof(failures)/sizeof(*failures);j++){kv_rc=failures[j];hint(120,16);assert(load_applied(&h,0,&i)==failures[j]);}
 puts("applied cursor: atomic proof, legacy upgrade and strict recovery refusal PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-applied-cursor-') as t:
 p=Path(t);(p/'test.c').write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
