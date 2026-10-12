#!/usr/bin/env python3
"""Separate the actual namespace participant and ancestry fixture envelopes."""
from pathlib import Path
import ast, os, shlex, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
fixture = Path(os.environ.get('EFS_NAMESPACE_ENVELOPE_SOURCE', root/'tests/live/gc_reclamation.py'))
host = (root/'src/server/raft_host.c').read_text()
start = host.index('static int host_namespace_capacity(')
end = host.index('\nstruct host_idx_ref', start)
code = r'''
#include <efs/txn.h>
#include <efs/raft.h>
#include <efs/meta_apply.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
struct efs_raft_host { struct efs_kv *kv; };
static efs_ino_t inodes[146],parents[146],next[4096];
static unsigned rows;
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint) {
 (void)h;(void)g;(void)hint;return EFS_OK;
}
int efs_meta_apply_get_inode(struct efs_kv *kv,efs_ino_t ino,struct efs_meta_row *r) {
 (void)kv;memset(r,0,sizeof(*r));r->mode=S_IFDIR|0755;
 for(unsigned i=0;i<rows;i++)if(inodes[i]==ino){r->parent=parents[i];return EFS_OK;}
 return EFS_ERR_NOT_FOUND;
}
int efs_txn_ver_get(struct efs_kv *kv,const uint8_t *key,uint32_t n,uint64_t *v) {
 (void)kv;(void)key;(void)n;*v=1;return EFS_OK;
}
''' + host[start:end] + r'''
static efs_ino_t allocate(uint32_t shard,efs_ino_t parent) {
 if(!next[shard])next[shard]=shard?shard:4096;
 if(next[shard]==EFS_ROOT_INO)next[shard]+=4096;
 efs_ino_t ino=next[shard];next[shard]+=4096;
 assert(rows<146);inodes[rows]=ino;parents[rows++]=parent;return ino;
}
static unsigned cardinality(const unsigned char *used) {
 unsigned n=0;for(unsigned i=0;i<4096;i++)n+=used[i]!=0;return n;
}
static void exercise(uint64_t salt,int selected) {
 memset(next,0,sizeof(next));rows=1;inodes[0]=parents[0]=1;
 unsigned char used[4096]={0};used[1]=1;
 efs_ino_t parent=1,moving=0;struct efs_raft_host h={0};
 for(int index=-1;index<144;index++) {
  char prefix[64],name[128];uint32_t shard=0;
  if(index<0)strcpy(prefix,"max-moving");else snprintf(prefix,sizeof(prefix),"max-%d",index);
  int fresh=index<142;
  if(selected) {
   unsigned attempt;
   for(attempt=0;attempt<65536;attempt++) {
    snprintf(name,sizeof(name),"%s-%u",prefix,attempt);
    shard=efs_kv_mkdir_shard(parent,name,salt);
    if((!used[shard])==fresh)break;
   }
   assert(attempt<65536);
  } else {strcpy(name,prefix);shard=efs_kv_mkdir_shard(parent,name,salt);}
  efs_ino_t destination=allocate(shard,parent);used[shard]=1;
  if(index<0){moving=destination;assert(cardinality(used)==2);continue;}
  struct efs_txn_parts parts={0};int hint=0,n=0;
  struct {struct host_pver_guard guards[144];uint64_t canary;} out;
  out.canary=0xabcdef9876543210ULL;
  /* LOCAL dentries share parent shards; rename also touches source inode.
   * Production ancestry admission itself adds every ancestor, including root. */
  assert(host_parts_add(&parts,efs_kv_inode_shard(parent))==EFS_OK);
  assert(host_parts_add(&parts,efs_kv_inode_shard(destination))==EFS_OK);
  assert(host_parts_add(&parts,efs_kv_inode_shard(moving))==EFS_OK);
  int rc=host_pver_guard_chain(&h,destination,moving,&parts,out.guards,&n,&hint);
  assert(out.canary==0xabcdef9876543210ULL);
  if(!selected && index==142) {
   assert(parent==2401 && destination==2385 && moving==2903);
   assert(cardinality(used)==145 && rc==EFS_ERR_OVERFLOW && parts.n==144 && n==143);
   printf("failed salt: parent2401 destination2385 source2903 root1; 145 participants, ancestry144; actual participant guard refuses PASS\n");
   return;
  }
  if(selected && index==143) {
   /* The root may be the final unvisited hop, so admitted participants can
    * be143 although the complete root/source/ancestry union is exactly144. */
   assert(cardinality(used)==144 && rc==EFS_ERR_OVERFLOW && parts.n<=144 && n==144);
   return;
  }
  assert(rc==EFS_OK);
  if(selected && index>=141)assert(cardinality(used)==144 && parts.n==144);
  parent=destination;
 }
 assert(0);
}
int main(void) {
 exercise(175431647951565505ULL,0);
 const uint64_t salts[]={175431647951565505ULL,0,1,UINT64_MAX,0x123456789abcdef0ULL,0xa55aa55aULL};
 for(unsigned i=0;i<sizeof(salts)/sizeof(salts[0]);i++)exercise(salts[i],1);
 puts("actual production hash/admission: six salts, 144 participants and144 ancestors accepted, 145 ancestors refused PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-namespace-envelope-') as tmp:
    source = Path(tmp)/'probe.c'
    binary = Path(tmp)/'probe'
    source.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc')) + ['-O1','-g','-Wall','-Wextra','-Werror','-D_GNU_SOURCE','-I'+str(root/'include'),str(source),str(root/'src/kv/kv_key.c'),'-o',str(binary)],check=True)
    subprocess.run([binary],check=True,timeout=10)

# The real fixture must use the verified construction in both variants, rather
# than keeping a salt-dependent plain-name path outside the fault variant.
tree = ast.parse(fixture.read_text())
block = next(n for n in ast.walk(tree) if isinstance(n,ast.If) and ast.unparse(n.test)=='a.namespace_max')
def assignment(nodes,name):
    return next((n.value for n in nodes if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id==name for t in n.targets)),None)
moving = assignment(block.body,'moving')
assert isinstance(moving,ast.Call) and isinstance(moving.func,ast.Name) and moving.func.id=='selected_directory', 'ordinary namespace fixture must select source placement, including root contribution'
assert assignment(block.body,'mkdir_salt') is not None, 'ordinary namespace fixture must read the actual export salt'
loop = next(n for n in block.body if isinstance(n,ast.For) and ast.unparse(n.iter)=='range(144)')
folder = assignment(loop.body,'folder')
assert isinstance(folder,ast.Call) and isinstance(folder.func,ast.Name) and folder.func.id=='selected_directory', 'ordinary namespace ancestry must separate participant and depth bounds'
assert ast.unparse(folder.args[-1])=='index < 142'
assert any(isinstance(n,ast.Assert) and ast.unparse(n.test)=='index == 143' for n in ast.walk(loop)), 'the original over-ancestry refusal index must remain unchanged'
print('ordinary and fault namespace fixture construction verified; limits and refusal index unchanged PASS')
