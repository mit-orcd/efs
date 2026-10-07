/* Isolated applied-state SM over mem KV. No sockets, no cluster. */
#include "efs/meta_apply.h"
#include "efs/metadata.h"
#include "efs/raft.h"
#include "efs/dir_layout.h"
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/meta_cmd.h"
#include "efs/opid.h"
#include "efs/txn.h"
#include "efs/session.h"
#include "efs/common.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* A fixed stamp, so a test can assert exact times. Apply takes the time as
 * an argument precisely so it is not the wall clock. */
#define T0 1000000000000000000ull

static const struct efs_meta_attrs g_at = { 1000, 1000, T0 };

static void fill_ch(struct efs_meta_chunk *ch)
{
    int i;

    memset(ch, 0, sizeof(*ch));
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        ch->nodes[i] = (efs_node_id_t)(i + 1);
        ch->checksums[i][0] = (uint8_t)(0x10 + i);
    }
}


int main(void) {
 struct efs_kv *kv=efs_kv_mem_create(); efs_ino_t ino=0;
 struct efs_meta_chunk ch,got; struct efs_meta_pub p={0}, first;
 struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX]; uint32_t nd=0; uint64_t newest=0;
 CHECK(efs_meta_apply_init(kv,T0)==EFS_OK,"init");
 CHECK(efs_meta_apply_create_file(kv,&g_at,EFS_ROOT_INO,S_IFREG|0644,"retry",&ino)==EFS_OK,"create");
 fill_ch(&ch); p.ino=ino;p.new_size=128*1024;p.coding_profile_id=EFS_META_PROFILE_K2F1;p.ch=ch;
 for(unsigned i=0;i<EFS_CHUNK_DELTA_MAX;i++) {
  p.delta_off=i;p.delta_len=1;p.candidate_gen=0xD1+i;
  CHECK(efs_meta_apply_publish(kv,&p)==EFS_OK,"initial span"); if(!i) first=p;
 }
 CHECK(efs_meta_apply_get_chunk_deltas(kv,ino,0,ds,EFS_CHUNK_DELTA_MAX,&nd,&newest)==EFS_OK,"list");
 p.delta_len=0;p.delta_base_n=nd;p.delta_base_seq=newest;p.candidate_gen=0xF1;
 CHECK(efs_meta_apply_publish(kv,&p)==EFS_OK,"fold");
 CHECK(efs_meta_apply_get_chunk_deltas(kv,ino,0,ds,EFS_CHUNK_DELTA_MAX,&nd,&newest)==EFS_OK,"folded list");
 p.expected_gen=0xF1;p.candidate_gen=0xF2;p.delta_base_n=nd;p.delta_base_seq=newest;p.ch.checksums[0][0]=0x77;
 CHECK(efs_meta_apply_publish(kv,&p)==EFS_OK,"later full overwrite");
 p.expected_gen=0;p.candidate_gen=0xE1;p.delta_off=1000;p.delta_len=1;
 CHECK(efs_meta_apply_publish(kv,&p)==EFS_OK,"churn evicts oldest folded identity");
 int rc=efs_meta_apply_publish(kv,&first);
 CHECK(efs_meta_apply_get_chunk(kv,ino,0,&got)==EFS_OK,"base after replay");
 CHECK(efs_meta_apply_get_chunk_deltas(kv,ino,0,ds,EFS_CHUNK_DELTA_MAX,&nd,&newest)==EFS_OK,"replayed list");
 unsigned reintroduced=0;for(unsigned i=0;i<nd;i++)if(ds[i].generation==first.candidate_gen&&ds[i].len)reintroduced++;
 printf("old-span=%llu retry-rc=%d later-base=%llu active-old-span=%u trailer-count=%u\n",(unsigned long long)first.candidate_gen,rc,(unsigned long long)got.generation,reintroduced,nd);
 CHECK(reintroduced==0,"ambiguous retry must not reintroduce old span after a later full overwrite");
 efs_kv_mem_free(kv);return failures?1:0;
}
