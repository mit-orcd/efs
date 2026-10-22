#include "efs/meta_apply.h"
#include "efs/session.h"
#include "efs/txn.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static void namespace_effects(void)
{
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);
 struct efs_meta_attrs at={.now=1};assert(!efs_meta_apply_init(kv,1));
 struct efs_opid_req q={.id={.session_epoch=1,.seq=1}};q.id.client_uuid[15]=127;
 assert(!efs_session_create(kv,q.id.client_uuid,1));assert(!efs_session_register(kv,q.id.client_uuid,1,1));
 assert(!efs_session_establish(kv,1,q.id.client_uuid,1));
 efs_ino_t ino=0,again=0,dir=0;struct efs_meta_dentry dent;struct efs_opid_reply rep;
 assert(!efs_meta_apply_create_file_log_op(kv,&at,1,S_IFREG|0600,"before",0,-1,&q,&ino));
 assert(!efs_meta_apply_create_file_log_op(kv,&at,1,S_IFREG|0600,"before",0,-1,&q,&again) && again==ino);
 assert(!efs_meta_apply_mkdir(kv,&at,1,S_IFDIR|0700,"dir",&dir));
 assert(!efs_session_begin_fence(kv,q.id.client_uuid));assert(!efs_session_fence_local(kv,1,q.id.client_uuid,2));
 assert(efs_meta_apply_opid_probe(kv,1,&q.id,&rep)==EFS_ERR_FENCED);
 assert(efs_meta_apply_create_file_log_op(kv,&at,1,S_IFREG|0600,"before",0,-1,&q,&again)==EFS_ERR_FENCED);
 q.id.seq=2;q.ack=1;
 assert(efs_meta_apply_create_file_log_op(kv,&at,1,S_IFREG|0600,"late",0,-1,&q,&again)==EFS_ERR_FENCED);
 assert(efs_meta_apply_lookup(kv,1,"late",&dent)==EFS_ERR_NOT_FOUND);
 assert(efs_meta_apply_mkdir_log_op(kv,&at,1,S_IFDIR|0700,"late-dir",0,-1,&q,&again)==EFS_ERR_FENCED);
 assert(efs_meta_apply_unlink_op(kv,1,"before",2,&q)==EFS_ERR_FENCED);
 assert(!efs_meta_apply_lookup(kv,1,"before",&dent) && dent.ino==ino);
 assert(efs_meta_apply_rmdir_op(kv,1,"dir",2,&q)==EFS_ERR_FENCED);
 assert(!efs_meta_apply_lookup(kv,1,"dir",&dent) && dent.ino==dir);
 /* K5: an identity never registered here is refused; no legacy effect. */
 q.id.client_uuid[15]=128;q.id.seq=1;q.ack=0;
 assert(efs_meta_apply_create_file_log_op(kv,&at,1,S_IFREG|0600,"legacy",0,-1,&q,&again)==EFS_ERR_UNREGISTERED);
 assert(efs_meta_apply_lookup(kv,1,"legacy",&dent)==EFS_ERR_NOT_FOUND);
 efs_kv_mem_free(kv);
}
static void shared_guards(void)
{
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);
 struct efs_opid_req q={.id={.session_epoch=1,.seq=1}};q.id.client_uuid[15]=113;
 assert(!efs_session_create(kv,q.id.client_uuid,1));assert(!efs_session_register(kv,q.id.client_uuid,1,7));
 assert(!efs_session_establish(kv,7,q.id.client_uuid,1));
 struct efs_txid first={{1}},second={{2}},late={{3}};
 struct efs_txn_parts parts={.n=1,.shard={7}},wrong={.n=1,.shard={8}};
 uint8_t key[EFS_KV_KEY_MAX];uint32_t bytes=0;struct efs_opid_reply reply={.rc=EFS_OK,.ino=123};
 assert(!efs_kv_key_opid(7,q.id.client_uuid,1,key,&bytes));
 assert(efs_txn_prepare_opid(kv,&first,&parts,NULL,bytes,&q,&reply)==EFS_ERR_INVAL);
 assert(efs_txn_prepare_opid(kv,&first,&wrong,key,bytes,&q,&reply)==EFS_ERR_INVAL);
 assert(!efs_txn_prepare_opid(kv,&first,&parts,key,bytes,&q,&reply));
 q.id.seq=2;assert(!efs_txn_prepare_opid(kv,&second,&parts,key,bytes,&q,&reply));
 assert(!efs_session_begin_fence(kv,q.id.client_uuid));
 assert(efs_session_fence_local(kv,7,q.id.client_uuid,2)==EFS_ERR_BUSY);
 assert(!efs_txn_resolve(kv,&first,7,EFS_TXN_COMMIT));
 assert(efs_session_fence_local(kv,7,q.id.client_uuid,2)==EFS_ERR_BUSY);
 assert(!efs_txn_resolve(kv,&second,7,EFS_TXN_ABORT));
 assert(!efs_session_fence_local(kv,7,q.id.client_uuid,2));
 assert(efs_txn_prepare_opid(kv,&late,&parts,key,bytes,&q,&reply)==EFS_ERR_FENCED);
 assert(!efs_txn_resolve(kv,&first,7,EFS_TXN_COMMIT));
 assert(efs_session_accept(kv,7,q.id.client_uuid,1)==EFS_ERR_FENCED);
 struct efs_opid_req foreign=q;foreign.id.client_uuid[0]=1;
 assert(efs_txn_prepare_opid(kv,&late,&parts,key,bytes,&foreign,&reply)==EFS_ERR_INVAL);
 efs_kv_mem_free(kv);
}
/* Exit without closing the LSM: each acknowledged guard/floor must recover
 * from its WAL, including the interval with one committed and one pending. */
static void cold_shared_guards(void)
{
 char root[]="/tmp/efs-namespace-guards-XXXXXX";assert(mkdtemp(root));
 struct efs_kv_lsm_cfg cfg={0};
 struct efs_opid_req q={.id={.session_epoch=1,.seq=1}};q.id.client_uuid[15]=114;
 struct efs_txid first={{31}},second={{32}},late={{33}};
 struct efs_txn_parts parts={.n=1,.shard={7}};
 for(unsigned phase=0;phase<4;phase++){
  pid_t pid=fork();assert(pid>=0);
  if(!pid){
   struct efs_kv *kv=efs_kv_lsm_open(root,&cfg);assert(kv);
   uint8_t key[EFS_KV_KEY_MAX];uint32_t bytes=0;struct efs_opid_reply reply={.rc=EFS_OK,.ino=123};
   assert(!efs_kv_key_opid(7,q.id.client_uuid,1,key,&bytes));
   if(!phase){
    assert(!efs_session_create(kv,q.id.client_uuid,1));assert(!efs_session_register(kv,q.id.client_uuid,1,7));
    assert(!efs_session_establish(kv,7,q.id.client_uuid,1));
    assert(!efs_txn_prepare_opid(kv,&first,&parts,key,bytes,&q,&reply));
    q.id.seq=2;assert(!efs_txn_prepare_opid(kv,&second,&parts,key,bytes,&q,&reply));
    assert(!efs_session_begin_fence(kv,q.id.client_uuid));
   }else if(phase==1){
    assert(efs_session_fence_local(kv,7,q.id.client_uuid,2)==EFS_ERR_BUSY);
    assert(!efs_txn_resolve(kv,&first,7,EFS_TXN_COMMIT));
    assert(efs_session_fence_local(kv,7,q.id.client_uuid,2)==EFS_ERR_BUSY);
   }else if(phase==2){
    assert(efs_session_fence_local(kv,7,q.id.client_uuid,2)==EFS_ERR_BUSY);
    assert(!efs_txn_resolve(kv,&second,7,EFS_TXN_ABORT));
    assert(!efs_session_fence_local(kv,7,q.id.client_uuid,2));
    assert(!efs_session_ack_fence(kv,q.id.client_uuid,7));assert(!efs_session_finish_fence(kv,q.id.client_uuid));
   }else{
    assert(efs_session_accept(kv,7,q.id.client_uuid,1)==EFS_ERR_FENCED);
    assert(!efs_txn_resolve(kv,&first,7,EFS_TXN_COMMIT));
    assert(!efs_txn_resolve(kv,&second,7,EFS_TXN_ABORT));
    assert(efs_txn_prepare_opid(kv,&late,&parts,key,bytes,&q,&reply)==EFS_ERR_FENCED);
    assert(efs_session_accept(kv,7,q.id.client_uuid,1)==EFS_ERR_FENCED);
   }
   _exit(0);
  }
  int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
 }
 DIR *dir=opendir(root);assert(dir);struct dirent *entry;
 while((entry=readdir(dir))){if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
  char path[1024];assert(snprintf(path,sizeof(path),"%s/%s",root,entry->d_name)<(int)sizeof(path));assert(!unlink(path));}
 assert(!closedir(dir) && !rmdir(root));
}
int main(void){namespace_effects();shared_guards();cold_shared_guards();puts("namespace session: fenced replay/effects refused; concurrent shared guards block revocation through commit/abort and delayed prepare; cold WAL guards/floors PASS");}
