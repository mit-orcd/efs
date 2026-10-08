#!/usr/bin/env python3
"""Production GC fan-out: all-member verdicts, fallback and joined lifetimes."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
job=s[s.index('struct gc_inode_job {'):s.index('static void *host_gc_inode_node(')]
wrapper=s[s.index('static int host_gc_inode_nodes('):s.index('\n/* Propose one GC command',s.index('static int host_gc_inode_nodes('))]
code=r'''
#include <efs/common.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
struct server {pthread_mutex_t lock;unsigned node_count;struct efs_node nodes[EFS_MAX_NODES];};
struct efs_raft_host {struct server *s;int n;};
static int active,peak,seen,failed,fail_create;
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
'''+job+r'''
static void *host_gc_inode_node(void *p) {
 struct gc_inode_job *j=p;
 pthread_mutex_lock(&guard);active++;if(active>peak)peak=active;seen|=1<<(j->node.id-1);pthread_mutex_unlock(&guard);
 usleep(5000);j->rc=j->node.id==(unsigned)failed?EFS_ERR_IO:EFS_OK;
 pthread_mutex_lock(&guard);active--;pthread_mutex_unlock(&guard);return 0;
}
static int create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *p) {
 return fail_create?11:pthread_create(t,a,fn,p);
}
#define pthread_create create
'''+wrapper+r'''
int main(void) {
 struct server s={.lock=PTHREAD_MUTEX_INITIALIZER,.node_count=4};
 struct efs_raft_host h={.s=&s,.n=4};for(int i=0;i<4;i++)s.nodes[i].id=i+1;
 assert(!host_gc_inode_nodes(&h,42,7));assert(seen==15&&peak<=4&&active==0);
 seen=peak=0;failed=4;
 assert(host_gc_inode_nodes(&h,42,7)==EFS_ERR_IO);assert(seen==15&&active==0);
 seen=peak=failed=0;s.node_count=3;
 assert(host_gc_inode_nodes(&h,42,7)==EFS_ERR_BUSY);assert(seen==7&&active==0);
 seen=peak=0;s.node_count=4;fail_create=1;
 assert(!host_gc_inode_nodes(&h,42,7));assert(seen==15&&peak==1&&active==0);
 seen=0;s.nodes[3].id=5;
 assert(host_gc_inode_nodes(&h,42,7)==EFS_ERR_BUSY&&seen==0&&active==0);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-gc-fanout-') as d:
 p=Path(d)/'gate.c';p.write_text(code)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-D_GNU_SOURCE','-I'+str(root/'include'),str(p),'-pthread','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('GC fan-out: bounded concurrency, all-member failure/missing refusal, fallback and joined lifetime PASS')
