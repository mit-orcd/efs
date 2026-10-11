#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include "efs/pack_mapping.h"
#include <stdio.h>
#include <assert.h>
#include <sys/stat.h>
struct stats {unsigned objects,phase[4],states[5],inodes,linked,orphan,lease,receipts,streams;};
static int scan(void *v,const uint8_t *key,uint32_t kl,const uint8_t *value,uint32_t vl){
 struct stats *s=v;if(kl<3)return 0;
 if(key[2]==EFS_KV_KIND_PACK_OBJECT){struct efs_pack_object o;assert(!efs_pack_object_decode(value,vl,&o));s->objects++;assert(o.ledger.phase<4);s->phase[o.ledger.phase]++;for(unsigned i=0;i<o.ledger.count;i++){assert(o.ledger.state[i]<5);s->states[o.ledger.state[i]]++;}}
 if(key[2]==EFS_KV_KIND_INODE){struct efs_meta_row r;assert(!efs_meta_unpack_inode(value,vl,&r));s->inodes++;s->linked+=r.nlink>0;}
 s->orphan+=key[2]==EFS_KV_KIND_ORPHAN;s->lease+=key[2]==EFS_KV_KIND_LEASE;s->streams+=key[2]==EFS_KV_KIND_PACK_STREAM;
 return 0;
}
int main(int argc,char **argv){assert(argc==2);struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_NOSYNC};struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);struct stats s={0};assert(!efs_kv_scan_prefix(kv,NULL,0,scan,&s));printf("objects=%u active=%u reclaiming=%u retired=%u pending=%u live=%u released=%u cancelled=%u inodes=%u linked=%u orphan=%u lease=%u streams=%u\n",s.objects,s.phase[1],s.phase[2],s.phase[3],s.states[1],s.states[2],s.states[3],s.states[4],s.inodes,s.linked,s.orphan,s.lease,s.streams);efs_kv_lsm_close(kv);return 0;}
