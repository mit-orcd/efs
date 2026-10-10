"""Compile the production read-only overlap classifier with a bounded log oracle."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
text=(root/'src/raft/raft.c').read_text()
a=text.index('static enum efs_batch_counter recv_overlap_shape(')
b=text.index('\n}',a)+2
code=r'''
#include <assert.h>
#include <stdint.h>
#include "efs/raft.h"
#include "efs/batch_obs.h"
struct efs_raft {uint64_t tail;int unavailable;};
static int last_log(struct efs_raft*r,uint64_t*i,uint64_t*t){*i=r->tail;*t=1;return EFS_OK;}
static int log_term(struct efs_raft*r,uint64_t i,uint64_t*t){if(r->unavailable||i>r->tail)return EFS_ERR_NOT_FOUND;*t=i?1:0;return EFS_OK;}
static int is_cfg_cmd(const uint8_t*p,uint32_t n){return n&&p[0]==193;}
'''+text[a:b]+r'''
int main(void){
 struct efs_raft r={3,0};struct efs_raft_msg m={0};uint8_t data=1,cfg=193;
 m.prev_index=1;m.prev_term=1;m.nentries=3;
 for(int i=0;i<3;i++){m.entries[i].term=1;m.entries[i].cmd=&data;m.entries[i].clen=1;}
 assert(recv_overlap_shape(&r,&m)==BO_OVERLAP_SUFFIX);
 m.nentries=2;assert(recv_overlap_shape(&r,&m)==BO_OVERLAP_DUP);
 m.entries[1].term=2;assert(recv_overlap_shape(&r,&m)==BO_OVERLAP_CONFLICT);m.entries[1].term=1;
 m.prev_term=2;assert(recv_overlap_shape(&r,&m)==BO_OVERLAP_CONFLICT);m.prev_term=1;
 m.entries[1].cmd=&cfg;assert(recv_overlap_shape(&r,&m)==BO_OVERLAP_CFG);m.entries[1].cmd=&data;
 r.unavailable=1;assert(recv_overlap_shape(&r,&m)==BO_OVERLAP_LOG);r.unavailable=0;
 m.prev_index=UINT64_MAX;assert(recv_overlap_shape(&r,&m)==BO_OVERLAP_LOG);
 m.prev_index=3;assert(recv_overlap_shape(&r,&m)==BO_OVERLAP_LOG);
 m.prev_index=0;m.prev_term=0;m.nentries=3;assert(recv_overlap_shape(&r,&m)==BO_OVERLAP_DUP);
 m.nentries=EFS_RAFT_AE_MAX+1;assert(recv_overlap_shape(&r,&m)==BO_OVERLAP_LOG);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-overlap-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('actual overlap classifier: duplicate/new suffix/conflict/config/log/overflow PASS')
