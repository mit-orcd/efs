#include "efs/dirty_ranges.h"
#include <assert.h>
#include <stdio.h>

#define CS 256u
static uint32_t rng = 71036;
static uint32_t random_u32(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void overlap_and_fence(void)
{
    struct efs_dirty_ranges r = {.chunk_size=CS}, old, masked, newer, saved;
    struct efs_fence_history h = {0};
    uint8_t local[CS], base[CS];
    memset(local,'A',CS);memset(base,'P',CS);
    assert(efs_dirty_ranges_write(&r,0,CS,0)==EFS_OK);
    old=r;
    assert(efs_fence_history_append(&h,1,100)==EFS_OK);
    /* Extension changes no epoch. A later partial write owns only its bytes. */
    memset(local+150,'B',50);
    assert(efs_dirty_ranges_write(&r,150,50,1)==EFS_OK);
    assert(r.count==3&&r.ranges[0].epoch==0&&r.ranges[1].epoch==1&&r.ranges[2].epoch==0);
    assert(efs_fence_history_append(&h,2,175)==EFS_OK);
    assert(efs_dirty_ranges_clip(&r,&h,2,0,0,&masked)==EFS_OK);
    assert(masked.count==2&&masked.ranges[0].len==100&&masked.ranges[1].len==25);
    assert(efs_dirty_ranges_overlay(&masked,base,CS,local)==EFS_OK);
    for(unsigned i=0;i<CS;i++) assert(base[i]==(i<100?'A':i>=150&&i<175?'B':'P'));
    /* A snapshot retains masks and ages despite later application writes. */
    newer=r;assert(efs_dirty_ranges_write(&newer,50,200,2)==EFS_OK);
    assert(efs_dirty_ranges_ack(&newer,&r)==EFS_ERR_STALE);
    assert(efs_dirty_ranges_ack(&r,&masked)==EFS_ERR_STALE);
    saved=masked;memset(&h,0,sizeof(h));
    assert(!memcmp(&saved,&masked,sizeof(saved)));
    /* A retired history without completeness for epoch zero fails closed. */
    assert(efs_dirty_ranges_clip(&old,&h,2,1,0,&masked)==EFS_ERR_STALE);
    assert(!memcmp(&saved,&masked,sizeof(saved)));
    assert(efs_dirty_ranges_ack(&r,&r)==EFS_OK&&r.count==0);
    assert(efs_dirty_ranges_write(&r,10,20,2)==EFS_OK);old=r;
    assert(efs_dirty_ranges_write(&r,10,20,2)==EFS_OK);
    assert(efs_dirty_ranges_ack(&r,&old)==EFS_ERR_STALE);
    assert(r.count==1); /* identical rewrite remains owned */
}

static void exhaustion_and_validation(void)
{
    struct efs_dirty_ranges r={.chunk_size=CS}, saved, out;
    struct efs_fence_history h={0};
    uint8_t body[CS], original[CS];
    for(unsigned i=0;i<EFS_DIRTY_RANGE_MAX;i++)
        assert(efs_dirty_ranges_write(&r,2*i,1,i)==EFS_OK);
    saved=r;
    assert(efs_dirty_ranges_write(&r,100,1,32)==EFS_ERR_BUSY);
    assert(!memcmp(&r,&saved,sizeof(r)));
    /* Splitting at capacity must not change ownership or accept new bytes. */
    struct efs_dirty_ranges split={.chunk_size=CS};
    assert(efs_dirty_ranges_write(&split,0,5,0)==EFS_OK);
    for(unsigned i=1;i<EFS_DIRTY_RANGE_MAX;i++)
        assert(efs_dirty_ranges_write(&split,8+2*i,1,i)==EFS_OK);
    saved=split;
    assert(efs_dirty_ranges_write(&split,2,1,32)==EFS_ERR_BUSY);
    assert(!memcmp(&split,&saved,sizeof(split)));
    /* A fully covering write can recover capacity without a drain. */
    assert(efs_dirty_ranges_write(&r,0,CS,32)==EFS_OK&&r.count==1);
    assert(efs_dirty_ranges_write(&r,100,20,33)==EFS_OK&&r.count==3);
    assert(r.ranges[0].len==100&&r.ranges[2].off==120);
    assert(efs_dirty_ranges_write(&r,100,20,32)==EFS_OK&&r.count==1);
    saved=r;
    assert(efs_dirty_ranges_write(&r,UINT32_MAX,2,0)==EFS_ERR_INVAL);
    assert(!memcmp(&r,&saved,sizeof(r)));
    r.mutation=UINT64_MAX;saved=r;
    assert(efs_dirty_ranges_write(&r,0,1,0)==EFS_ERR_BUSY);
    assert(!memcmp(&r,&saved,sizeof(r)));
    memset(&out,0xA5,sizeof(out));saved=out;
    assert(efs_dirty_ranges_clip(&r,&h,31,0,0,&out)==EFS_ERR_STALE);
    assert(!memcmp(&out,&saved,sizeof(out)));
    assert(efs_dirty_ranges_clip(&r,&h,32,0,UINT64_MAX-CS+1,&out)==EFS_ERR_INVAL);
    assert(!memcmp(&out,&saved,sizeof(out)));
    memset(body,'X',CS);memcpy(original,body,CS);
    r.ranges[0].len=CS+1;
    assert(efs_dirty_ranges_overlay(&r,body,CS,original)==EFS_ERR_INVAL);
    assert(!memcmp(body,original,CS));
    r=saved;r.chunk_size=CS;r.count=EFS_DIRTY_RANGE_MAX+1;
    assert(efs_dirty_ranges_valid(&r)==EFS_ERR_INVAL);
}

/* Independent byte ownership model: each successful write updates exact
 * byte ages; each fence permanently kills bytes written before its epoch
 * beyond its size. A later extension never revives them. */
static void byte_model(void)
{
    struct efs_dirty_ranges r={.chunk_size=CS};
    struct efs_fence_history h={0};
    uint64_t ages[CS]={0}, epoch=0;
    uint8_t owned[CS]={0}, alive[CS]={0}, local[CS]={0}, merged[CS];
    unsigned busy=0, writes=0;
    const uint64_t chunk_start=1024;
    for(unsigned step=0;step<50000;step++) {
        unsigned action=random_u32()%10;
        if(action==0&&h.count<EFS_FENCE_HISTORY_MAX) {
            uint64_t size=chunk_start+random_u32()%(CS+1);
            epoch++;
            assert(efs_fence_history_append(&h,epoch,size)==EFS_OK);
            for(unsigned i=0;i<CS;i++) if(owned[i]&&ages[i]<epoch&&chunk_start+i>=size)alive[i]=0;
        } else if(action<8) {
            unsigned off=random_u32()%CS,len=1+random_u32()%(CS-off);
            struct efs_dirty_ranges before=r;
            int rc=efs_dirty_ranges_write(&r,off,len,epoch);
            if(rc==EFS_ERR_BUSY) { assert(!memcmp(&before,&r,sizeof(r)));busy++; }
            else {
                assert(rc==EFS_OK);writes++;
                for(unsigned i=off;i<off+len;i++) {
                    owned[i]=alive[i]=1;ages[i]=epoch;local[i]=(uint8_t)(1+step%254);
                }
            }
        } else if(action==8) {
            struct efs_dirty_ranges snap=r;
            assert(efs_dirty_ranges_ack(&r,&snap)==EFS_OK);
            memset(owned,0,CS);memset(alive,0,CS);
        }
        assert(efs_dirty_ranges_valid(&r)==EFS_OK);
        /* Check exact ownership ages independently of clipping. */
        for(unsigned i=0;i<CS;i++) {
            unsigned matches=0;
            for(unsigned j=0;j<r.count;j++) {
                const struct efs_fence_part *p=&r.ranges[j];
                if(i>=p->off&&i-p->off<p->len) { matches++;assert(owned[i]&&ages[i]==p->epoch); }
            }
            assert(matches==owned[i]);
        }
        struct efs_dirty_ranges masked;
        assert(efs_dirty_ranges_clip(&r,&h,epoch,0,chunk_start,&masked)==EFS_OK);
        memset(merged,255,CS);
        assert(efs_dirty_ranges_overlay(&masked,merged,CS,local)==EFS_OK);
        for(unsigned i=0;i<CS;i++) assert(merged[i]==(owned[i]&&alive[i]?local[i]:255));
    }
    assert(writes>10000);(void)busy;
}
int main(void)
{
    overlap_and_fence();exhaustion_and_validation();byte_model();
    puts("dirty ranges: mixed epochs, overlap/split, bounded admission, stale ACK and 50000-step byte model PASS");
}
