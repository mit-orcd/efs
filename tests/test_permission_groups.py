#!/usr/bin/env python3
"""Real permission-group helper: oversized libfuse counts cannot overread."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
s = Path(os.environ.get('EFS_FUSE_SOURCE', root / 'src/client/efs_fuse.c')).read_text()
m = re.search(r'^static int caller_in_group\([^;]+?\)\n\{', s, re.M)
function = s[m.start():s.index('\n}', m.end()) + 2]
code = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
struct fuse_ctx {gid_t gid;};
static struct fuse_ctx context={1001};
static int have_context=1;
static void *t_req=(void*)1;
static const struct fuse_ctx *ll_ctx(void){return have_context?&context:NULL;}
static int first_count,second_count,calls,no_memory;
static int fuse_req_getgroups(void *req,int size,gid_t *list){
 assert(req);int count=calls++?second_count:first_count;
 for(int i=0;i<size && i<count;i++)list[i]=(gid_t)(2000+i);
 return count;
}
static void *fixture_malloc(size_t n){return no_memory?NULL:malloc(n);}
#define malloc fixture_malloc
''' + function + r'''
#undef malloc
static void reset(int first,int second){first_count=first;second_count=second;calls=0;no_memory=0;have_context=1;t_req=(void*)1;}
int main(void){
 (void)fixture_malloc;
 reset(100,100);assert(caller_in_group(1001) && calls==0);
 have_context=0;assert(!caller_in_group(2000) && calls==0);
 reset(100,100);t_req=NULL;assert(!caller_in_group(2000) && calls==0);
 reset(0,0);assert(!caller_in_group(2000) && calls==1);
 reset(4,4);assert(caller_in_group(2002) && calls==1);
 reset(4,4);assert(!caller_in_group(2090) && calls==1);
 reset(100,100);assert(caller_in_group(2090) && calls==2);
 reset(100,100);assert(!caller_in_group(9999) && calls==2);
 reset(100,110);assert(!caller_in_group(2002) && calls==2);
 reset(100,3);assert(caller_in_group(2002) && calls==2);
 reset(100,-EIO);assert(!caller_in_group(2002) && calls==2);
 reset(-ENOSYS,0);assert(!caller_in_group(2002) && calls==1);
 reset(70000,70000);assert(!caller_in_group(2002) && calls==1);
 reset(100,100);no_memory=1;assert(!caller_in_group(2090) && calls==1);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-permission-groups-') as d:
    path = Path(d) / 'test.c'
    path.write_text(code)
    subprocess.run(['cc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True,timeout=15)
print('Permission groups: large supplementary membership, bounds, changed lists, API/allocation failures and sanitizer checks PASS')
