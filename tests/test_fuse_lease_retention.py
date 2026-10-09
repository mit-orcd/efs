#!/usr/bin/env python3
"""Execute production lease bookkeeping through ambiguous HOLD replies."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/efs_fuse.c').read_text()
def function(name):
 m=re.search(r'^static (?:int|void) '+name+r'\([^;]+?\)\n\{',s,re.M)
 assert m,name
 return s[m.start():s.index('\n}',m.end())+2]
a=s.index('static pthread_mutex_t g_open_mu')
b=s.index('/* Inodes this client has actually locked.',a)
source=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
typedef uint64_t efs_ino_t;
#define EFS_OK 0
#define EFS_ERR_NOMEM -12
#define EFS_ERR_BUSY -16
#define EFS_ERR_NET -5
#define EFS_ERR_NOT_FOUND -3
static struct {int export_id;uint64_t flock_token;} g_client={1,123};
static int open_rc,close_rc,opens,closes,server_hold;
static int efs_client_rpc_hold(int export_id,efs_ino_t ino,int acquire,uint64_t token) {
 assert(export_id==1&&ino==42&&token==123);
 assert(!pthread_mutex_trylock(&g_open_mu));pthread_mutex_unlock(&g_open_mu);
 if(acquire){opens++;server_hold=1;return open_rc;}
 closes++; if(!close_rc||close_rc==EFS_ERR_NOT_FOUND)server_hold=0; return close_rc;
}
'''
# RPC mock references the production mutex, so place bookkeeping first.
mock=source[source.index('static int efs_client_rpc_hold'):]
source=source[:source.index('static int efs_client_rpc_hold')]+s[a:b]+mock
for name in ('efs_open_note','efs_open_set_leased','efs_close_note','efs_open_release_pending','efs_open_drain_pending','efs_open_acquire'):
 source+='\n'+function(name)
source+=r'''
int main(void) {
 assert(!efs_open_acquire(42));assert(!efs_open_acquire(42));assert(opens==1);
 int leased=0;assert(!efs_close_note(42,&leased));assert(!leased);
 assert(efs_close_note(42,&leased)&&leased);
 close_rc=EFS_ERR_NET;assert(efs_open_release_pending(42)==EFS_ERR_NET);
 assert(g_open_refs&&!g_open_refs->n&&g_open_refs->close_pending&&server_hold);
 assert(efs_open_acquire(42)==EFS_ERR_NET&&opens==1);
 close_rc=0;assert(!efs_open_acquire(42));assert(opens==2&&g_open_refs->n==1&&!g_open_refs->close_pending);
 assert(efs_close_note(42,&leased));close_rc=EFS_ERR_NET;
 assert(efs_open_drain_pending()==EFS_ERR_NET&&g_open_refs);
 close_rc=0;assert(efs_open_drain_pending()==EFS_ERR_BUSY&&!g_open_refs&&!server_hold);
 assert(!efs_open_drain_pending());
 open_rc=EFS_ERR_NET;close_rc=EFS_ERR_NET;
 assert(efs_open_acquire(42)==EFS_ERR_NET&&server_hold&&g_open_refs&&!g_open_refs->n);
 int prior=opens;assert(efs_open_acquire(42)==EFS_ERR_NET&&opens==prior);
 close_rc=0;assert(efs_open_drain_pending()==EFS_ERR_BUSY&&!g_open_refs&&!server_hold);
 open_rc=0;assert(!efs_open_acquire(42));assert(efs_close_note(42,&leased));
 assert(!efs_open_release_pending(42)&&!g_open_refs);
 assert(!efs_open_acquire(42));assert(efs_close_note(42,&leased));
 close_rc=EFS_ERR_NOT_FOUND;assert(!efs_open_release_pending(42)&&!g_open_refs&&!server_hold);
 puts("FUSE ambiguous OPEN/CLOSE: retained ownership, reopen barrier and clean-stop retry PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-lease-retention-') as d:
 p=Path(d)/'test.c';p.write_text(source)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
