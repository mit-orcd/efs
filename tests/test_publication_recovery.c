/* Exact outcomes survive lost replies, superseding writes, fences and crash. */
#include "efs/publication.h"
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include "efs/session.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static struct efs_meta_pub publication(efs_ino_t ino,uint64_t gen,uint64_t base,uint64_t object)
{
    struct efs_meta_pub p={0};p.ino=ino;p.inode_gen=gen;p.candidate_gen=object;
    p.expected_gen=base;p.new_size=64;p.now=100;p.coding_profile_id=EFS_META_PROFILE_K2F1;
    p.durable_result=1;p.publication_id.client_uuid[0]=1;
    p.publication_id.session_epoch=1;p.publication_id.seq=object;
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++){p.ch.nodes[i]=i+1;p.ch.checksums[i][0]=(uint8_t)object;}
    return p;
}
static void admit(struct efs_kv *kv,efs_ino_t ino)
{
    uint8_t uuid[EFS_OPID_UUID_LEN]={1};
    uint32_t shard=efs_kv_lane_shard(ino,0);
    assert(efs_session_create(kv,uuid,1)==EFS_OK);
    assert(efs_session_register(kv,uuid,1,shard)==EFS_OK);
    assert(efs_session_establish(kv,shard,uuid,1)==EFS_OK);
}
static int result(struct efs_kv *kv,const struct efs_meta_pub *p,int *verdict)
{
    uint8_t digest[EFS_HASH_SIZE];assert(efs_publication_digest(p,digest)==EFS_OK);
    return efs_meta_publication_result(kv,p,verdict);
}
static void exercise(struct efs_kv *kv,efs_ino_t ino,uint64_t gen)
{
    admit(kv,ino);
    struct efs_meta_pub first=publication(ino,gen,0,10),loser=publication(ino,gen,0,11);
    struct efs_meta_pub next=publication(ino,gen,10,12);int v=123;
    assert(result(kv,&first,&v)==EFS_ERR_NOT_FOUND && v==123);
    assert(efs_meta_apply_publish(kv,&first)==EFS_OK);
    /* Aggregate STALE can now hide first's success, but each result is exact. */
    assert(efs_meta_apply_publish(kv,&loser)==EFS_ERR_STALE);
    assert(result(kv,&first,&v)==EFS_OK && v==EFS_OK);
    assert(result(kv,&loser,&v)==EFS_OK && v==EFS_ERR_STALE);
    assert(efs_meta_apply_publish(kv,&next)==EFS_OK);
    assert(efs_meta_apply_publish(kv,&first)==EFS_OK); /* reply lost, object superseded */
    struct efs_meta_chunk got;assert(efs_meta_apply_get_chunk(kv,ino,0,&got)==EFS_OK && got.generation==12);
    struct efs_meta_pub same=next;same.expected_gen=12;same.publication_id.seq=99;
    assert(efs_meta_apply_publish(kv,&same)==EFS_OK); /* new identical write, distinct operation */
    assert(result(kv,&same,&v)==EFS_OK && v==EFS_OK);
    struct efs_meta_pub changed=same;changed.new_size++;
    assert(efs_meta_apply_publish(kv,&changed)==EFS_ERR_PROTO); /* identity cannot change payload */
    assert(efs_meta_apply_content_fence(kv,ino,gen,EFS_META_FENCE_INODE,0,1,0,200)==EFS_OK);
    assert(efs_meta_apply_publish(kv,&first)==EFS_OK); /* do not resurrect after fence */
    assert(efs_meta_apply_publish(kv,&loser)==EFS_ERR_STALE);
    assert(efs_meta_apply_get_chunk(kv,ino,0,&got)==EFS_OK && got.generation==12);
    struct efs_meta_pub fresh=first;fresh.content_epoch=1;fresh.candidate_gen=13;fresh.expected_gen=12;fresh.publication_id.seq=13;
    assert(result(kv,&fresh,&v)==EFS_ERR_NOT_FOUND);
    uint8_t a[EFS_HASH_SIZE],b[EFS_HASH_SIZE];assert(efs_publication_digest(&first,a)==EFS_OK);
    first.now++;first.mtime_gen++;first.lane_local=1;
    assert(efs_publication_digest(&first,b)==EFS_OK && !memcmp(a,b,sizeof(a)));
    first.new_size++;assert(efs_publication_digest(&first,b)==EFS_OK && memcmp(a,b,sizeof(a)));
}
static int fault_batch(void *ctx,const struct efs_kv_item *items,uint32_t n)
{(void)ctx;(void)items;(void)n;return EFS_ERR_IO;}
static void identical_span_operations(struct efs_kv *kv,efs_ino_t ino,uint64_t gen)
{
    admit(kv,ino);
    struct efs_meta_pub span=publication(ino,gen,0,21);span.delta_len=16;
    assert(efs_meta_apply_publish(kv,&span)==EFS_OK);
    struct efs_meta_delta deltas[EFS_CHUNK_DELTA_MAX];uint32_t n;uint64_t newest;
    assert(efs_meta_apply_get_chunk_deltas(kv,ino,0,deltas,EFS_CHUNK_DELTA_MAX,&n,&newest)==EFS_OK && n==1);
    struct efs_meta_pub peer=publication(ino,gen,0,22);peer.delta_base_n=n;peer.delta_base_seq=newest;
    assert(efs_meta_apply_publish(kv,&peer)==EFS_OK);
    assert(efs_meta_apply_publish(kv,&span)==EFS_OK); /* original retry cannot reorder */
    assert(efs_meta_apply_get_chunk_deltas(kv,ino,0,deltas,EFS_CHUNK_DELTA_MAX,&n,&newest)==EFS_OK && n==1 && !deltas[0].len);
    span.publication_id.seq=23;span.expected_gen=22; /* application writes same bytes again */
    assert(efs_meta_apply_publish(kv,&span)==EFS_OK);
    assert(efs_meta_apply_get_chunk_deltas(kv,ino,0,deltas,EFS_CHUNK_DELTA_MAX,&n,&newest)==EFS_OK && n==2 && deltas[1].generation==21 && deltas[1].len==16);
}
static void failures(void)
{
    struct efs_kv *kv=efs_kv_mem_create();assert(kv);struct efs_meta_attrs at={0,0,1};efs_ino_t ino;
    struct efs_meta_row row;assert(efs_meta_apply_init(kv,1)==EFS_OK);
    assert(efs_meta_apply_create_file(kv,&at,EFS_ROOT_INO,S_IFREG|0644,"fault",&ino)==EFS_OK);
    assert(efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK);
    admit(kv,ino);
    struct efs_meta_pub p=publication(ino,row.generation,0,10);int v;
    struct efs_kv_ops ops=*kv->ops;ops.batch=fault_batch;
    struct efs_kv failing={&ops,kv->ctx};
    assert(efs_meta_apply_publish(&failing,&p)==EFS_ERR_IO);
    assert(result(kv,&p,&v)==EFS_ERR_NOT_FOUND);
    struct efs_meta_chunk got;assert(efs_meta_apply_get_chunk(kv,ino,0,&got)==EFS_ERR_NOT_FOUND);
    assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
    struct efs_meta_pub stale=publication(ino,row.generation,0,11);
    assert(efs_meta_apply_publish(&failing,&stale)==EFS_ERR_IO);
    assert(result(kv,&stale,&v)==EFS_ERR_NOT_FOUND);
    assert(efs_meta_apply_get_chunk(kv,ino,0,&got)==EFS_OK && got.generation==10);
    assert(efs_meta_apply_publish(kv,&stale)==EFS_ERR_STALE);
    assert(result(kv,&stale,&v)==EFS_OK && v==EFS_ERR_STALE);
    /* Existing mapping without receipt must not fabricate an earlier success. */
    struct efs_meta_pub legacy=publication(ino,row.generation,10,20);legacy.durable_result=0;
    assert(efs_meta_apply_publish(kv,&legacy)==EFS_OK);legacy.durable_result=1;
    assert(efs_meta_apply_publish(kv,&legacy)==EFS_ERR_STALE);
    assert(result(kv,&legacy,&v)==EFS_OK && v==EFS_ERR_STALE);
    efs_ino_t other;struct efs_meta_row other_row;
    assert(efs_meta_apply_create_file(kv,&at,EFS_ROOT_INO,S_IFREG|0644,"spans",&other)==EFS_OK);
    assert(efs_meta_apply_get_inode(kv,other,&other_row)==EFS_OK);
    identical_span_operations(kv,other,other_row.generation);
    efs_kv_mem_free(kv);
}
static void retirement(struct efs_kv *kv,efs_ino_t ino,uint64_t gen)
{
    struct efs_meta_pub p=publication(ino,gen,0,10),q=publication(ino,gen,0,11);int v;
    assert(efs_meta_apply_publication_retire(kv,&q)==EFS_ERR_BUSY); /* first unacknowledged */
    struct efs_meta_pub changed=p;changed.new_size++;
    assert(efs_meta_apply_publication_retire(kv,&changed)==EFS_ERR_PROTO);
    assert(efs_kv_mem_fail_next_batch(kv)==EFS_OK);
    assert(efs_meta_apply_publication_retire(kv,&p)==EFS_ERR_IO);
    assert(result(kv,&p,&v)==EFS_OK && v==EFS_OK);
    assert(efs_meta_apply_publication_retire(kv,&p)==EFS_OK);
    assert(result(kv,&p,&v)==EFS_OK && v==EFS_META_PUBLICATION_RETIRED);
    assert(efs_meta_apply_publish(kv,&p)==EFS_ERR_STALE);
    assert(efs_meta_apply_publication_retire(kv,&p)==EFS_OK);
    assert(efs_meta_apply_publication_retire(kv,&q)==EFS_OK);
    assert(efs_meta_apply_check(kv)==EFS_OK);
}
static void bounded_receipts(void)
{
    struct efs_kv *kv=efs_kv_mem_create();assert(kv);struct efs_meta_attrs at={0,0,1};
    efs_ino_t ino;struct efs_meta_row row;
    assert(efs_meta_apply_init(kv,1)==EFS_OK);
    assert(efs_meta_apply_create_file(kv,&at,EFS_ROOT_INO,S_IFREG|0644,"bounded",&ino)==EFS_OK);
    assert(efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK);
    admit(kv,ino);
    struct efs_meta_pub first=publication(ino,row.generation,0,10);
    assert(efs_meta_apply_publish(kv,&first)==EFS_OK);
    for(unsigned i=1;i<EFS_META_PUBLICATION_MAX_RECEIPTS;i++) {
        struct efs_meta_pub p=publication(ino,row.generation,0,10+i);
        assert(efs_meta_apply_publish(kv,&p)==EFS_ERR_STALE);
    }
    struct efs_meta_pub extra=publication(ino,row.generation,0,1000);int v;
    assert(efs_meta_apply_publish(kv,&extra)==EFS_ERR_BUSY);
    assert(result(kv,&extra,&v)==EFS_ERR_NOT_FOUND);
    assert(efs_meta_apply_publication_retire(kv,&first)==EFS_OK);
    assert(efs_meta_apply_publish(kv,&extra)==EFS_ERR_STALE);
    /* A fresh identity below the acknowledged floor cannot be admitted. */
    first.publication_id.seq=5;assert(efs_meta_apply_publish(kv,&first)==EFS_ERR_STALE);
    assert(efs_meta_apply_check(kv)==EFS_OK);efs_kv_mem_free(kv);
}
int main(void)
{
    struct efs_kv *mem=efs_kv_mem_create();assert(mem);struct efs_meta_attrs ma={0,0,1};efs_ino_t mi;struct efs_meta_row mr;
    assert(efs_meta_apply_init(mem,1)==EFS_OK);assert(efs_meta_apply_create_file(mem,&ma,EFS_ROOT_INO,S_IFREG|0644,"retire",&mi)==EFS_OK);
    assert(efs_meta_apply_get_inode(mem,mi,&mr)==EFS_OK);exercise(mem,mi,mr.generation);retirement(mem,mi,mr.generation);efs_kv_mem_free(mem);
    failures();bounded_receipts();char dir[]="/tmp/efs-publication-XXXXXX";assert(mkdtemp(dir));
    struct efs_kv *kv=efs_kv_lsm_open(dir,NULL);assert(kv);
    struct efs_meta_attrs at={0,0,1};efs_ino_t ino;struct efs_meta_row row;
    assert(efs_meta_apply_init(kv,1)==EFS_OK);
    assert(efs_meta_apply_create_file(kv,&at,EFS_ROOT_INO,S_IFREG|0644,"file",&ino)==EFS_OK);
    assert(efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK);efs_kv_lsm_close(kv);
    pid_t child=fork();assert(child>=0);
    if(!child){kv=efs_kv_lsm_open(dir,NULL);assert(kv);exercise(kv,ino,row.generation);_exit(0);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    kv=efs_kv_lsm_open(dir,NULL);assert(kv);int v;
    struct efs_meta_pub first=publication(ino,row.generation,0,10),loser=publication(ino,row.generation,0,11);
    assert(result(kv,&first,&v)==EFS_OK && v==EFS_OK);
    assert(result(kv,&loser,&v)==EFS_OK && v==EFS_ERR_STALE);
    assert(efs_meta_apply_publish(kv,&first)==EFS_OK);
    struct efs_meta_chunk got;assert(efs_meta_apply_get_chunk(kv,ino,0,&got)==EFS_OK && got.generation==12);
    efs_kv_lsm_close(kv);child=fork();assert(child>=0);
    if(!child){kv=efs_kv_lsm_open(dir,NULL);assert(kv);assert(efs_meta_apply_publication_retire(kv,&first)==EFS_OK);_exit(0);}
    assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    kv=efs_kv_lsm_open(dir,NULL);assert(kv);
    assert(result(kv,&first,&v)==EFS_OK && v==EFS_META_PUBLICATION_RETIRED);
    assert(efs_meta_apply_publish(kv,&first)==EFS_ERR_STALE);
    assert(efs_meta_apply_get_chunk(kv,ino,0,&got)==EFS_OK && got.generation==12);
    assert(efs_meta_apply_check(kv)==EFS_OK);efs_kv_lsm_close(kv);
    DIR *d=opendir(dir);assert(d);struct dirent *e;
    while((e=readdir(d))){if(e->d_name[0]=='.')continue;char p[512];snprintf(p,sizeof(p),"%s/%s",dir,e->d_name);assert(!unlink(p));}
    closedir(d);assert(!rmdir(dir));puts("publication atomic outcomes and crash recovery PASS");return 0;
}
