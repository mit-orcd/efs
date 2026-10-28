/* Golden bytes, unaligned input and exact-size refusal for shared codecs.
 * Also cover the client geometry and range helpers after removing copies. */
#include "efs/byteorder.h"
#include "efs/meta_setattr_command.h"
#include "efs/writer_ranges.h"
#include "../src/client/client_internal.h"
#include "../src/kv/kv_order.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct efs_client g_client;
int main(void)
{
    uint8_t b[80];
    const uint64_t values[]={0,1,UINT32_MAX,UINT64_C(0x0102030405060708),UINT64_MAX};
    for(unsigned off=0;off<8;off++) for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++) {
        memset(b,0xa5,sizeof(b));
        uint64_t v=values[i];efs_store_be64(b+off,v);
        assert(efs_load_be64(b+off)==v && b[off+8]==0xa5);
        for(unsigned k=0;k<8;k++) assert(b[off+k]==(uint8_t)(v>>(56-8*k)));
        efs_store_be32(b+off,(uint32_t)v);assert(efs_load_be32(b+off)==(uint32_t)v);
        for(unsigned k=0;k<4;k++) assert(b[off+k]==(uint8_t)(v>>(24-8*k)));
        efs_store_le32(b+off,(uint32_t)v);assert(efs_load_le32(b+off)==(uint32_t)v);
        for(unsigned k=0;k<4;k++) assert(b[off+k]==(uint8_t)(v>>(8*k)));
    }
    uint8_t golden[EFS_META_SETATTR_COMMAND_LEN];golden[0]=EFS_MD_CMD_SETATTR;
    for(unsigned i=1;i<sizeof(golden);i++) golden[i]=(uint8_t)i;
    struct efs_meta_setattr_command q;
    assert(!efs_meta_setattr_command_decode(golden,sizeof(golden),&q));
    assert(q.ino==UINT64_C(0x0102030405060708) && q.now==UINT64_C(0x090a0b0c0d0e0f10));
    assert(q.attrs.expect_gen==UINT64_C(0x1112131415161718) && q.attrs.mask==UINT32_C(0x191a1b1c));
    assert(q.attrs.mode==UINT32_C(0x1d1e1f20) && q.attrs.uid==UINT32_C(0x21222324) && q.attrs.gid==UINT32_C(0x25262728));
    assert(q.uuid[0]==41 && q.uuid[15]==56 && q.epoch==UINT32_C(0x393a3b3c));
    memset(b,0xa5,sizeof(b));
    efs_meta_setattr_command_encode(b+1,q.ino,q.now,&q.attrs,q.uuid,q.epoch);
    assert(!memcmp(b+1,golden,sizeof(golden)) && b[0]==0xa5 && b[sizeof(golden)+1]==0xa5);
    struct efs_meta_setattr_command saved=q;
    for(unsigned len=0;len<sizeof(golden);len++) {
        /* Exact allocation makes the short-input checks useful under ASan. */
        uint8_t *short_input=malloc(len?len:1);assert(short_input);
        memcpy(short_input,golden,len);
        assert(efs_meta_setattr_command_decode(short_input,len,&q)==EFS_ERR_UNSUPPORTED);
        assert(!memcmp(&q,&saved,sizeof(q)));free(short_input);
    }
    assert(efs_meta_setattr_command_decode(b+1,sizeof(golden)+1,&q)==EFS_ERR_UNSUPPORTED);
    golden[0]=255;assert(efs_meta_setattr_command_decode(golden,sizeof(golden),&q)==EFS_ERR_UNSUPPORTED);
    assert(!memcmp(&q,&saved,sizeof(q)));
    assert(efs_meta_setattr_command_decode(NULL,0,&q)==EFS_ERR_UNSUPPORTED);
    assert(efs_meta_setattr_command_decode(b+1,EFS_META_SETATTR_COMMAND_LEN,NULL)==EFS_ERR_UNSUPPORTED);

    assert(efs_client_chunk_size()==EFS_DEFAULT_CHUNK_SIZE);
    g_client.export.chunk_size=EFS_MIN_CHUNK_SIZE;assert(efs_client_frag_size()==EFS_MIN_CHUNK_SIZE/2);
    g_client.export.chunk_size=EFS_MAX_CHUNK_SIZE;assert(efs_client_chunk_size()==EFS_MAX_CHUNK_SIZE);
    g_client.export.chunk_size=3;assert(efs_client_chunk_size()==EFS_DEFAULT_CHUNK_SIZE);
    uint8_t *frags[EFS_NUM_FRAGMENTS];efs_client_frag_ptrs(b,8,frags);
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++)assert(frags[i]==b+8*i);
    assert(efs_writer_buffers_overlap(b,8,b+7,8));
    assert(!efs_writer_buffers_overlap(b,8,b+8,8));
    assert(!efs_writer_buffers_overlap(b,0,b,8));
    assert(kv_key_cmp(NULL,0,NULL,0)==0);
    assert(kv_key_cmp((const uint8_t *)"a",1,(const uint8_t *)"aa",2)<0);
    assert(kv_key_cmp((const uint8_t *)"\xff",1,(const uint8_t *)"a",1)>0);
    for(unsigned shard=0;shard<65536;shard++) {
        uint8_t k[2]={(uint8_t)(shard>>8),(uint8_t)shard};
        assert(kv_key_in_group(k,2,0)==(shard<4096 && (shard&1)));
        assert(kv_key_in_group(k,2,2)==(shard<4096 && !(shard&1)));
        assert(!kv_key_in_group(k,1,0));
    }
    puts("shared codecs, SETATTR golden bytes/refusal, client geometry and KV ordering PASS");
    return 0;
}
