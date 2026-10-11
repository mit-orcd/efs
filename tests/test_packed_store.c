/* Actual files-backend PUT/GET and namespace/accounting gates for W95 packs.
 * This private test uses one host, not a distributed/RDMA acceptance verdict. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "efs/packed_segment.h"
static int fail_file, fail_directory, fail_after_unlink;
static unsigned file_syncs, directory_syncs;
static int fault_sync(int fd)
{
    struct stat st; assert(!fstat(fd,&st));
    if (S_ISREG(st.st_mode)) {
        file_syncs++;
        if (fail_file) {fail_file--; errno=EIO;return -1;}
    } else if (S_ISDIR(st.st_mode)) {
        directory_syncs++;
        if (fail_directory) {fail_directory--; errno=EIO;return -1;}
    }
    return fsync(fd);
}
static int fault_unlink(const char *path)
{
    int rc = unlink(path);
    if (!rc && fail_after_unlink) { fail_after_unlink = 0; fail_directory = 1; }
    return rc;
}
#define unlink fault_unlink
#define fsync fault_sync
#include "../src/server/store.c"
#undef fsync
#undef unlink
static struct efsd_server server;
static int cleanup(const char *path,const struct stat *st,int kind,struct FTW *f)
{ (void)st;(void)f;return kind==FTW_DP?rmdir(path):unlink(path); }
int main(void)
{
    struct efs_pack_record records[16]; struct efs_pack_ref refs[16];
    uint8_t payload[512]; memset(payload,0x37,sizeof(payload));
    for (unsigned i=0;i<16;i++) records[i]=(struct efs_pack_record){.ino=100+i,
        .inode_generation=17,.content_epoch=23,.record_generation=29+i,.len=512,.data=payload};
    uint8_t *image=malloc(EFS_SEGMENT_MAX_SIZE),*decoded=malloc(EFS_SEGMENT_MAX_SIZE);
    uint8_t *frags,*read;assert(image&&decoded);
    assert(!posix_memalign((void **)&frags,4096,EFS_SEGMENT_MAX_SIZE*3/2));
    assert(!posix_memalign((void **)&read,4096,EFS_SEGMENT_MAX_SIZE*3/2));
    size_t size;assert(!efs_pack_build(501,503,7,records,16,image,EFS_SEGMENT_MAX_SIZE,&size,refs));
    uint32_t flen=(uint32_t)size/2;
    uint8_t *f[3]={frags,frags+flen,frags+2*flen};
    assert(!efs_encode_segment(image,size,size,f));
    uint8_t sums[3][EFS_HASH_SIZE];for(unsigned i=0;i<3;i++)efs_hash(f[i],flen,sums[i]);
    for(int direct=0;direct<=1;direct++) {
        char root[]="/tmp/efs-packed-store-XXXXXX";assert(mkdtemp(root));
        memset(&server,0,sizeof(server));server.id=1;server.node_count=1;server.nodes[0].id=1;
        server.direct_io=direct;server.storage_path_count=2;server.quota=8<<20;
        pthread_mutex_init(&server.lock,NULL);
        for(unsigned r=0;r<2;r++) {
            snprintf(server.storage_paths[r],EFS_MAX_PATH,"%s/r%u",root,r);
            assert(!mkdir(server.storage_paths[r],0755));
        }
        strcpy(server.storage_path,server.storage_paths[0]);assert(!server_storage_engine_init(&server));
        efs_tls_write_root=0;
        uint64_t allocated=0;
        for(unsigned i=0;i<3;i++) {
            assert(!server_write_packed_fragment(&server,1,501,503,i,f[i],flen,sums[i]));
            char path[8192];assert(!server_packed_fragment_path(&server,0,1,501,503,i,path,sizeof(path)));
            struct stat st;assert(!stat(path,&st));
            assert(st.st_size==(off_t)flen+(direct?4096:32));
            allocated+=(uint64_t)st.st_blocks*512;
        }
        assert(server.nodes[0].used==3u*flen);
        assert(server_delete_packed_fragment(&server,1,501,503,0,flen,sums[0])==EFS_ERR_BUSY);
        /* Idempotent retry neither repads nor double charges, even with rerouting. */
        efs_tls_write_root=1;file_syncs=directory_syncs=0;
        assert(!server_write_packed_fragment(&server,1,501,503,0,f[0],flen,sums[0]));
        assert(file_syncs && directory_syncs && server.nodes[0].used==3u*flen);
        char absent[8192];assert(!server_packed_fragment_path(&server,1,1,501,503,0,absent,sizeof(absent)));
        assert(access(absent,F_OK)<0&&errno==ENOENT);
        uint8_t wrong[EFS_HASH_SIZE];memcpy(wrong,sums[0],sizeof(wrong));wrong[0]^=1;
        assert(server_write_packed_fragment(&server,1,501,503,0,f[0],flen,wrong)==EFS_ERR_CHECKSUM);
        f[0][0]^=1;efs_hash(f[0],flen,wrong);
        assert(server_write_packed_fragment(&server,1,501,503,0,f[0],flen,wrong)==EFS_ERR_STALE);
        f[0][0]^=1;
        /* An ordinary inode's death fence cannot delete or forbid a pack ID. */
        int gc;do{gc=server_gc_inode(&server,1,501,17);}while(gc==EFS_ERR_BUSY);assert(gc==EFS_OK);
        server_storage_engine_close(&server);assert(!server_storage_engine_init(&server));
        server_init_local_usage(&server);assert(server.nodes[0].used==3u*flen);
        uint8_t *r[3]={read,read+flen,read+2*flen};
        for(unsigned i=0;i<3;i++) {
            uint32_t cap=flen;uint8_t sum[EFS_HASH_SIZE];
            assert(!server_read_packed_fragment(&server,1,501,503,i,flen,r[i],&cap,sum));
            assert(cap==flen&&!memcmp(sum,sums[i],EFS_HASH_SIZE));
        }
        for(int missing=0;missing<3;missing++) {
            memset(r[missing],0xa5,flen);
            assert(!efs_decode_segment(r,size,(missing+1)%3,(missing+2)%3,missing,decoded,size));
            for(unsigned i=0;i<16;i++) {
                const uint8_t *data;uint32_t len;
                assert(!efs_pack_read(decoded,size,&refs[i],&data,&len));
                assert(len==512&&!memcmp(data,payload,len));
            }
            memcpy(r[missing],f[missing],flen);
        }
        /* Matched physical allocation control: sixteen separate legacy files. */
        uint8_t *legacy; assert(!posix_memalign((void **)&legacy,4096,EFS_MIN_CHUNK_SIZE*3/2));
        uint8_t *lf[3]={legacy,legacy+EFS_MIN_CHUNK_SIZE/2,legacy+EFS_MIN_CHUNK_SIZE};
        assert(!efs_encode_chunk(payload,sizeof(payload),EFS_MIN_CHUNK_SIZE,lf));
        struct efs_export ex={.id=1,.chunk_size=EFS_MIN_CHUNK_SIZE};
        uint64_t legacy_allocated=0;
        for(unsigned i=0;i<16;i++) for(unsigned fi=0;fi<3;fi++) {
            uint8_t sum[EFS_HASH_SIZE];efs_hash(lf[fi],EFS_MIN_CHUNK_SIZE/2,sum);
            efs_tls_chunk_gen=701+i;
            assert(!server_write_fragment_with_sum_sync(&server,&ex,1001+i,0,fi,lf[fi],EFS_MIN_CHUNK_SIZE/2,sum));
            char path[8192];server_fragment_path(&server,&ex,1001+i,0,fi,path,sizeof(path));
            struct stat st;assert(!stat(path,&st));legacy_allocated+=(uint64_t)st.st_blocks*512;
        }
        free(legacy);
        assert(allocated && legacy_allocated/allocated>=20);
        printf("PACK_CONTROL direct=%d records=16 payload=8192 legacy_allocated_bytes=%llu packed_allocated_bytes=%llu reduction=%.6f\n",
            direct,(unsigned long long)legacy_allocated,(unsigned long long)allocated,(double)legacy_allocated/allocated);
        /* A fully written file with failed barrier is not an acknowledged PUT. */
        uint64_t before=server.nodes[0].used;fail_file=1;
        assert(server_write_packed_fragment(&server,1,511,513,0,f[0],flen,sums[0])==EFS_ERR_IO);
        assert(server.nodes[0].used==before+flen);
        fail_file=1;
        assert(server_write_packed_fragment(&server,1,511,513,0,f[0],flen,sums[0])==EFS_ERR_IO);
        fail_directory=1;
        assert(server_write_packed_fragment(&server,1,511,513,0,f[0],flen,sums[0])==EFS_ERR_IO);
        assert(!server_write_packed_fragment(&server,1,511,513,0,f[0],flen,sums[0]));
        assert(server.nodes[0].used==before+flen);
        server.quota=server.nodes[0].used;
        assert(server_write_packed_fragment(&server,1,521,523,0,f[0],flen,sums[0])==EFS_ERR_QUOTA);
        assert(!server_write_packed_fragment(&server,1,501,503,0,f[0],flen,sums[0]));
        assert(server_write_packed_fragment(&server,1,501,503,0,f[0],flen-1,sums[0])==EFS_ERR_INVAL);
        /* Duplicate object locations fail closed rather than hide accounting. */
        char source[8192];assert(!server_packed_fragment_path(&server,0,1,501,503,0,source,sizeof(source)));
        char *slash=strrchr(absent,'/');assert(slash);*slash=0;make_dir_up(absent);*slash='/';
        assert(!link(source,absent));uint32_t cap=flen;
        assert(server_read_packed_fragment(&server,1,501,503,0,flen,r[0],&cap,wrong)==EFS_ERR_IO);
        assert(!unlink(absent));
        /* Typed generation fences block every late PUT, including retries
         * after failed file/directory barriers. No ACK without both barriers. */
        server.quota=0;fail_file=1;
        assert(server_fence_packed_object(&server,1,901,903)==EFS_ERR_IO);
        for(unsigned fi=0;fi<3;fi++)
            assert(server_write_packed_fragment(&server,1,901,903,fi,f[fi],flen,sums[fi])==EFS_ERR_STALE);
        fail_file=1;assert(server_fence_packed_object(&server,1,901,903)==EFS_ERR_IO);
        fail_directory=1;assert(server_fence_packed_object(&server,1,901,903)==EFS_ERR_IO);
        file_syncs=directory_syncs=0;assert(!server_fence_packed_object(&server,1,901,903));
        assert(file_syncs && directory_syncs);
        assert(!server_fence_packed_object(&server,1,501,503));
        /* Fencing changes upload admission, not existing reader bytes. */
        cap=flen;assert(!server_read_packed_fragment(&server,1,501,503,0,flen,r[0],&cap,wrong));
        /* Actual typed deletion: refuse wrong evidence and retain accounting
         * across an unlink followed by failed directory sync and lost ACK. */
        memcpy(wrong,sums[0],sizeof(wrong));wrong[0]^=1;
        uint64_t used_before=server.nodes[0].used;
        assert(server_delete_packed_fragment(&server,1,501,503,0,flen,wrong)==EFS_ERR_CHECKSUM);
        assert(server.nodes[0].used==used_before);
        fail_after_unlink=1;
        assert(server_delete_packed_fragment(&server,1,501,503,0,flen,sums[0])==EFS_ERR_IO);
        assert(server.nodes[0].used==used_before-flen);
        file_syncs=directory_syncs=0;
        assert(!server_delete_packed_fragment(&server,1,501,503,0,flen,sums[0]));
        assert(file_syncs && directory_syncs && server.nodes[0].used==used_before-flen);
        for(unsigned fi=1;fi<3;fi++)
            assert(!server_delete_packed_fragment(&server,1,501,503,fi,flen,sums[fi]));
        for(unsigned fi=0;fi<3;fi++) {
            uint32_t missing_capacity=flen;
            assert(server_read_packed_fragment(&server,1,501,503,fi,flen,r[fi],&missing_capacity,wrong)==EFS_ERR_NOT_FOUND);
            assert(!server_delete_packed_fragment(&server,1,501,503,fi,flen,sums[fi]));
        }
        assert(server.nodes[0].used==used_before-3u*flen);
        server_storage_engine_close(&server);assert(!server_storage_engine_init(&server));
        for(unsigned fi=0;fi<3;fi++)
            assert(server_write_packed_fragment(&server,1,501,503,fi,f[fi],flen,sums[fi])==EFS_ERR_STALE);
        /* Ordinary inode death, another export and a new segment generation
         * are separate namespaces; the old typed fence must not poison them. */
        assert(!server_write_packed_fragment(&server,1,501,505,0,f[0],flen,sums[0]));
        assert(!server_write_packed_fragment(&server,2,501,503,0,f[0],flen,sums[0]));
        char fence[8192];assert(!packed_fence_path(&server,1,901,903,fence,sizeof(fence)));
        int fd=open(fence,O_WRONLY);assert(fd>=0);uint8_t broken=0xff;
        assert(pwrite(fd,&broken,1,0)==1);assert(!close(fd));
        assert(server_fence_packed_object(&server,1,901,903)==EFS_ERR_IO);
        assert(server_write_packed_fragment(&server,1,901,903,0,f[0],flen,sums[0])==EFS_ERR_IO);
        printf("PACK_STORE direct=%d records=16 payload=8192 fragment_payload_bytes=%u allocated_bytes=%llu\n",
            direct,3*flen,(unsigned long long)allocated);
        server_storage_engine_close(&server);pthread_mutex_destroy(&server.lock);
        assert(!nftw(root,cleanup,32,FTW_DEPTH|FTW_PHYS));
    }
    free(image);free(decoded);free(frags);free(read);
    puts("packed store: explicit geometry, immutable retries, barriers, cold EC reads, quota, inode-GC isolation, durable typed upload fences and actual checksum-bound GC retries PASS");
}
