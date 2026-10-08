#!/usr/bin/env python3
"""Execute production CREATE/OPEN callbacks with rejected kernel replies."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/efs_fuse.c').read_text()
def function(name):
 m=re.search(r'^static void '+name+r'\([^;]+?\)\n\{',s,re.M)
 assert m,name
 return s[m.start():s.index('\n}',m.end())+2]
source=r'''
#include <assert.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
typedef uint64_t fuse_ino_t;
typedef void *fuse_req_t;
struct fuse_file_info { uint64_t fh; int flags; };
struct fuse_entry_param { uint64_t ino; };
struct efs_inode { uint64_t ino; };
static void *t_req;
static int operation_rc,lookup_rc,reply_rc,has_created,release_count,error_count,reply_count;
static uint64_t released_ino;
static void lookup_memo_mutation_begin(void) {}
static void lookup_memo_mutation_end(void) {}
static int efs_fuse_create_at(fuse_ino_t p,const char *n,mode_t m,struct fuse_file_info *fi,void *out,struct efs_inode *created) {
 (void)p;(void)n;(void)m;(void)out;created->ino=has_created?42:0;
 if(!operation_rc) { fi->fh=42; } return operation_rc;
}
static int efs_fuse_open_ino(fuse_ino_t ino,struct fuse_file_info *fi) {if(!operation_rc)fi->fh=ino;return operation_rc;}
static uint64_t efs_file_ino(struct fuse_file_info *fi,uint64_t fallback) {return fi->fh?fi->fh:fallback;}
static void fill_stat_from_inode(struct stat *st,const struct efs_inode *row) {(void)st;(void)row;}
static void fill_entry(fuse_ino_t ino,struct fuse_entry_param *e,const struct stat *st) {(void)st;e->ino=ino;}
static int lookup_fill(fuse_ino_t ino,struct fuse_entry_param *e,struct fuse_file_info *fi) {(void)fi;e->ino=ino;return lookup_rc;}
static int efs_fuse_release_ino(fuse_ino_t ino,struct fuse_file_info *fi) {assert(fi->fh);release_count++;released_ino=fi->fh;assert(!ino||ino==fi->fh);fi->fh=0;return 0;}
static void fuse_reply_err(fuse_req_t req,int err) {(void)req;assert(err);error_count++;}
static int fuse_reply_create(fuse_req_t req,const struct fuse_entry_param *e,struct fuse_file_info *fi) {(void)req;assert(e->ino==fi->fh);reply_count++;return reply_rc;}
static int fuse_reply_open(fuse_req_t req,struct fuse_file_info *fi) {(void)req;assert(fi->fh);reply_count++;return reply_rc;}
'''+function('ll_open_run')+'\n'+function('ll_create_run')+r'''
int main(void) {
 for(int existing=0;existing<2;existing++)for(int rejected=0;rejected<2;rejected++) {
  struct fuse_file_info fi={0};has_created=!existing;reply_rc=rejected?-2:0;
  release_count=error_count=reply_count=0;operation_rc=lookup_rc=0;
  ll_create_run(NULL,1,"f",0600,&fi);
  assert(release_count==rejected&&reply_count==1&&!error_count);
  assert(rejected?!fi.fh:fi.fh==42);assert(!t_req);
 }
 struct fuse_file_info fi={0};has_created=0;operation_rc=0;lookup_rc=-5;reply_rc=0;
 release_count=error_count=reply_count=0;ll_create_run(NULL,1,"f",0600,&fi);
 assert(release_count==1&&released_ino==42&&error_count==1&&!reply_count&&!fi.fh);
 fi.fh=0;operation_rc=-12;release_count=error_count=reply_count=0;
 ll_create_run(NULL,1,"f",0600,&fi);assert(!release_count&&error_count==1&&!reply_count);
 for(int truncated=0;truncated<2;truncated++)for(int rejected=0;rejected<2;rejected++) {
  fi=(struct fuse_file_info){.flags=truncated?O_TRUNC:0};operation_rc=0;reply_rc=rejected?-2:0;
  release_count=error_count=reply_count=0;ll_open_run(NULL,42,&fi);
  assert(release_count==rejected&&reply_count==1&&!error_count&&!t_req);
 }
 puts("FUSE CREATE/OPEN: rejected replies and failed reply construction release exactly once PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-open-reply-') as d:
 p=Path(d)/'test.c';p.write_text(source)
 flags=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror']
 subprocess.run(flags+[str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
