#!/usr/bin/env python3
"""Execute the FUSE ownership-change authorization helper with actual identities."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_FUSE_SOURCE',root/'src/client/efs_fuse.c')).read_text()
m=re.search(r'^static int check_chown_perm\([^;]+?\)\n\{',s,re.M)
function=s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <efs/metadata.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
struct fuse_ctx {uid_t uid;gid_t gid;};
static struct fuse_ctx context={1001,2001};
static int have_context=1;
static const struct fuse_ctx *ll_ctx(void){return have_context?&context:NULL;}
static int caller_in_group(gid_t group){return group==2001 || group==2002;}
'''+function+r'''
int main(void){
 struct efs_inode row={0};row.uid=3001;row.gid=4001;
 assert(check_chown_perm(&row,(uid_t)-1,(gid_t)-1)==-EPERM);
 assert(check_chown_perm(&row,3001,4001)==-EPERM);
 assert(check_chown_perm(&row,3001,(gid_t)-1)==-EPERM);
 assert(check_chown_perm(&row,(uid_t)-1,4001)==-EPERM);
 assert(check_chown_perm(&row,(uid_t)-1,2001)==-EPERM);
 row.uid=context.uid;
 assert(!check_chown_perm(&row,(uid_t)-1,(gid_t)-1));
 assert(!check_chown_perm(&row,1001,4001));
 assert(!check_chown_perm(&row,(uid_t)-1,2001));
 assert(!check_chown_perm(&row,(uid_t)-1,2002));
 assert(check_chown_perm(&row,(uid_t)-1,5001)==-EPERM);
 assert(check_chown_perm(&row,3001,2001)==-EPERM);
 context.uid=0;assert(!check_chown_perm(&row,3001,5001));
 have_context=0;assert(!check_chown_perm(&row,3001,5001));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-chown-permission-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('Chown permissions: nonowners refused even for unchanged IDs; owner groups, root and internal calls PASS')
