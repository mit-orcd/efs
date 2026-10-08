/* Real efsd fragment entry points and physical container persistence faults. */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int fault;
static const char *watched_root;
static unsigned outside_syncs, root_syncs, mkdir_calls, mapping_reads;
static ssize_t container_pread(int fd,void *buf,size_t n,off_t off) {
    struct stat st;assert(!fstat(fd,&st));
    if(st.st_size==4096 && n==4096 && off==0)mapping_reads++;
    return pread(fd,buf,n,off);
}
static int container_mkdir(const char *path, mode_t mode) {
    mkdir_calls++;
    return mkdir(path,mode);
}
static int container_sync(int fd)
{
    struct stat st; assert(!fstat(fd,&st));
    char p[64],name[8192];snprintf(p,sizeof(p),"/proc/self/fd/%d",fd);
    ssize_t n=readlink(p,name,sizeof(name)-1);assert(n>=0);name[n]=0;
    if(watched_root && S_ISDIR(st.st_mode)) {
        if(!strcmp(name,watched_root))root_syncs++;
        else if(strncmp(name,watched_root,strlen(watched_root)))outside_syncs++;
    }
    int body=strstr(name,".efs-containers/")&&strstr(name,".dat");
    if(fault==1&&body){errno=EIO;return -1;}
    if(fault==2&&S_ISDIR(st.st_mode)){errno=EIO;return -1;}
    int rc=fsync(fd);
    if(fault==4&&body)_exit(44);
    return rc;
}
static int container_punch(int fd,int mode,off_t off,off_t len)
{
    if(fault==3&&(mode&FALLOC_FL_PUNCH_HOLE)){errno=EIO;return -1;}
    return fallocate(fd,mode,off,len);
}
static int container_rename(const char *a,const char *b)
{
    int rc=rename(a,b);if(!rc&&fault==5)_exit(45);return rc;
}
static int container_renameat(int ad,const char *a,int bd,const char *b)
{
    int rc=renameat(ad,a,bd,b);if(!rc&&fault==5)_exit(45);return rc;
}
#define pread container_pread
#define renameat container_renameat
#define mkdir container_mkdir
#define fsync container_sync
#define fallocate container_punch
#define rename container_rename
#include "../src/server/store_containers.c"
#undef pread
#undef renameat
#undef mkdir
#undef fsync
#undef fallocate
#undef rename
#include "../src/server/store.c"
static struct efsd_server server;
static struct efs_export ex={.id=1,.chunk_size=EFS_DEFAULT_CHUNK_SIZE};
static void write_value(efs_ino_t ino,unsigned ci,unsigned char value,int expected)
{
    uint8_t data[EFS_FRAGMENT_SIZE],sum[32];memset(data,value,sizeof(data));efs_hash(data,sizeof(data),sum);
    assert(server_write_fragment_with_sum_sync(&server,&ex,ino,ci,0,data,sizeof(data),sum)==expected);
}
static void read_value(efs_ino_t ino,unsigned ci,int expected,int value)
{
    uint8_t data[EFS_FRAGMENT_SIZE],sum[32];uint32_t len=sizeof(data);int ok=0;
    int rc=server_read_fragment_with_sum(&server,&ex,ino,ci,0,data,&len,sum,&ok);assert(rc==expected);
    if(!rc){assert(ok==(efs_ino_is_meta_table(ino)?EFS_STORE_SUM_PRESENT:EFS_STORE_SUM_VERIFIED)&&len==sizeof(data));uint8_t h[32];efs_hash(data,len,h);assert(!memcmp(sum,h,32));
        for(unsigned i=0;i<len;i++)assert(data[i]==(value<0?data[0]:(unsigned)value));}
}
static void restart(void){server_storage_engine_close(&server);assert(!server_storage_engine_init(&server));server_init_local_usage(&server);}
static void *actor(void *arg)
{
    efs_tls_chunk_gen=333;efs_tls_write_root=0;
    for(unsigned i=0;i<25;i++){write_value(4242,0,(uintptr_t)arg,EFS_OK);read_value(4242,0,EFS_OK,-1);}return NULL;
}
static int remove_owned(const char *p,const struct stat *st,int type,struct FTW *f)
{(void)st;(void)f;return type==FTW_DP?rmdir(p):unlink(p);}
int main(void)
{
    for(int direct=0;direct<=1;direct++) {
        char base[]="/tmp/efs-containers-XXXXXX";assert(mkdtemp(base));
        memset(&server,0,sizeof(server));server.id=1;server.node_count=1;server.nodes[0].id=1;
        server.storage_engine=1;server.direct_io=direct;server.quota=1ull<<30;server.storage_path_count=2;
        pthread_mutex_init(&server.lock,NULL);
        for(unsigned r=0;r<2;r++){snprintf(server.storage_paths[r],EFS_MAX_PATH,"%s/r%u",base,r);assert(!mkdir(server.storage_paths[r],0755));}
        strcpy(server.storage_path,server.storage_paths[0]);assert(!server_storage_engine_init(&server));
        struct efsd_server *probe=calloc(1,sizeof(*probe));assert(probe);
        probe->storage_engine=1;probe->storage_path_count=1;strcpy(probe->storage_paths[0],server.storage_paths[0]);
        assert(server_storage_engine_init(probe)==EFS_ERR_IO);free(probe);
        efs_tls_write_root=0;efs_tls_chunk_gen=100;
        write_value(42,0,17,EFS_OK);read_value(42,0,EFS_OK,17);assert(server.nodes[0].used==EFS_FRAGMENT_SIZE);
        /* Idempotent same identity is charged once; replacement uses a fresh extent. */
        int cached_fd=((struct target*)server.container_targets[0])->data_fds[0];assert(cached_fd>=0);
        assert(!!(fcntl(cached_fd,F_GETFL)&O_DIRECT)==direct);
        watched_root=server.storage_paths[0];outside_syncs=root_syncs=mkdir_calls=0;
        write_value(42,0,18,EFS_OK);assert(server.nodes[0].used==EFS_FRAGMENT_SIZE);
        assert(!outside_syncs && root_syncs && !mkdir_calls);watched_root=NULL;
        assert(((struct target*)server.container_targets[0])->data_fds[0]==cached_fd);
        /* One validated mapping serves quota admission and old-extent retirement. */
        mapping_reads=0;write_value(42,0,18,EFS_OK);assert(mapping_reads==1);
        /* A nonce collision must fail without altering the existing temp/map. */
        char collision_path[8192],collision_tmp[8192];
        server_fragment_path(&server,&ex,42,0,0,collision_path,sizeof(collision_path));
        struct target *ct=target_at(&server,collision_path);struct mapping cm;assert(!mapping_read(collision_path,ct,&cm));
        snprintf(collision_tmp,sizeof(collision_tmp),"%s.tmp.%08x%08x",collision_path,get32(cm.page+24),get32(cm.page+28));
        int collision_fd=open(collision_tmp,O_WRONLY|O_CREAT|O_EXCL,0600);assert(collision_fd>=0);
        assert(write(collision_fd,"keep",4)==4);close(collision_fd);
        int collision_published=0;assert(atomic_page(collision_path,cm.page,PAGE,&collision_published,ct)==EFS_ERR_IO);
        assert(!collision_published);collision_fd=open(collision_tmp,O_RDONLY);char keep[4];assert(collision_fd>=0);
        assert(read(collision_fd,keep,4)==4&&!memcmp(keep,"keep",4));close(collision_fd);assert(!unlink(collision_tmp));read_value(42,0,EFS_OK,18);
        efs_tls_chunk_gen=101;write_value(42,0,19,EFS_OK);
        efs_tls_chunk_gen=100;read_value(42,0,EFS_OK,18);
        efs_tls_write_root=1;write_value(84,0,20,EFS_OK);
        write_value(EFS_META_TABLE_INO,0,21,EFS_OK);read_value(EFS_META_TABLE_INO,0,EFS_OK,21);
        write_value(EFS_META_TABLE_INO+7,0,22,EFS_OK);read_value(EFS_META_TABLE_INO+7,0,EFS_OK,22);
        uint64_t usage=server.nodes[0].used;assert(usage==3*EFS_FRAGMENT_SIZE);restart();assert(server.nodes[0].used==usage);
        read_value(84,0,EFS_OK,20);efs_tls_write_root=0;read_value(42,0,EFS_OK,18);
        uint8_t wrong[32]={0},data[EFS_FRAGMENT_SIZE],sum[32];memset(data,18,sizeof(data));efs_hash(data,sizeof(data),sum);
        assert(server_delete_fragment_if_sum(&server,&ex,42,0,0,wrong)==EFS_ERR_EXIST);read_value(42,0,EFS_OK,18);
        assert(!server_delete_fragment_if_sum(&server,&ex,42,0,0,sum));read_value(42,0,EFS_ERR_NOT_FOUND,0);
        assert(!server_delete_fragment_if_sum(&server,&ex,42,0,0,sum));
        efs_tls_chunk_gen=101;read_value(42,0,EFS_OK,19);
        /* Enforce quota before publishing or replacing a map. */
        server.quota=server.nodes[0].used;write_value(126,0,33,EFS_ERR_QUOTA);server.quota=1ull<<30;
        efs_tls_chunk_gen=222;write_value(126,0,34,EFS_OK);
        fault=1;write_value(126,0,35,EFS_ERR_IO);fault=0;read_value(126,0,EFS_OK,34);restart();read_value(126,0,EFS_OK,34);
        /* Failed final namespace barrier is an unknown outcome, never success. */
        fault=2;write_value(126,0,36,EFS_ERR_IO);fault=0;restart();read_value(126,0,EFS_OK,36);
        /* Kill after body persistence and after map rename: recover a whole version. */
        for(int stage=4;stage<=5;stage++){
            pid_t child=fork();assert(child>=0);
            if(!child){fault=stage;write_value(126,0,(unsigned char)(40+stage),EFS_OK);_exit(1);}
            int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==40+stage);
            restart();read_value(126,0,EFS_OK,stage==4?36:45);
        }
        /* Failed punching retains allocation; restart releases unreachable ranges. */
        fault=3;write_value(126,0,46,EFS_OK);fault=0;restart();read_value(126,0,EFS_OK,46);
        memset(data,46,sizeof(data));efs_hash(data,sizeof(data),sum);fault=3;
        assert(server_delete_fragment_if_sum(&server,&ex,126,0,0,sum)==EFS_ERR_IO);fault=0;
        restart();read_value(126,0,EFS_ERR_NOT_FOUND,0);
        /* Readers and writers of the same range cannot observe mixed records. */
        pthread_t threads[4];for(uintptr_t i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,actor,(void*)(i+50)));
        for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
        efs_tls_chunk_gen=333;read_value(4242,0,EFS_OK,-1);restart();read_value(4242,0,EFS_OK,-1);
        assert(!server_gc_inode(&server,1,4242,9));read_value(4242,0,EFS_ERR_NOT_FOUND,0);write_value(4242,0,60,EFS_ERR_STALE);
        /* Bounded inode orphan cleanup and persistent death fence. */
        efs_tls_chunk_gen=444;for(unsigned ci=0;ci<70;ci++)write_value(777,ci,61,EFS_OK);
        int gr;do{gr=server_gc_inode(&server,1,777,9);}while(gr==EFS_ERR_BUSY);assert(gr==EFS_OK);
        restart();write_value(777,71,61,EFS_ERR_STALE);
        /* A large variable fragment and full-segment rollover use another container. */
        struct target *full=server.container_targets[0];
        memset(full->bits[0],255,BLOCKS/8); /* test-only allocator pressure */
        uint32_t large_len=EFS_MAX_FRAGMENT_SIZE;
        uint8_t *large=malloc(large_len),*large_read=malloc(large_len),large_sum[32];assert(large&&large_read);
        memset(large,77,large_len);efs_hash(large,large_len,large_sum);efs_tls_chunk_gen=888;
        assert(!server_write_fragment_with_sum_sync(&server,&ex,900,0,0,large,large_len,large_sum));
        uint32_t cap=large_len;int sum_ok=0;assert(!server_read_fragment_with_sum(&server,&ex,900,0,0,large_read,&cap,sum,&sum_ok));
        assert(cap==large_len&&sum_ok&&!memcmp(large,large_read,cap));restart();
        cap=large_len;assert(!server_read_fragment_with_sum(&server,&ex,900,0,0,large_read,&cap,sum,&sum_ok));assert(!memcmp(large,large_read,cap));
        assert(!server_delete_fragment_if_sum(&server,&ex,900,0,0,large_sum));free(large);free(large_read);
        /* A segment outside the descriptor cache retains per-operation I/O. */
        full=server.container_targets[0];full->cursor_seg=CACHED_SEGS;full->cursor_block=0;
        efs_tls_chunk_gen=889;write_value(901,0,78,EFS_OK);read_value(901,0,EFS_OK,78);
        restart();read_value(901,0,EFS_OK,78);memset(data,78,sizeof(data));efs_hash(data,sizeof(data),sum);
        assert(!server_delete_fragment_if_sum(&server,&ex,901,0,0,sum));
        /* Short payloads preserve zeroed alignment padding after copy changes. */
        uint8_t small[17];memset(small,79,sizeof(small));efs_hash(small,sizeof(small),sum);efs_tls_chunk_gen=890;
        assert(!server_write_fragment_with_sum_sync(&server,&ex,902,0,0,small,sizeof(small),sum));
        char small_path[8192],small_body[8192];server_fragment_path(&server,&ex,902,0,0,small_path,sizeof(small_path));
        struct target *small_t=target_at(&server,small_path);struct mapping small_map;assert(!mapping_read(small_path,small_t,&small_map));
        segpath(small_t,small_map.seg,small_body);int small_fd=open(small_body,O_RDONLY);assert(small_fd>=0);
        uint8_t padded[PAGE];assert(pread(small_fd,padded,PAGE,(off_t)small_map.off*PAGE+PAGE)==PAGE);close(small_fd);
        assert(!memcmp(padded,small,sizeof(small)));for(unsigned i=sizeof(small);i<PAGE;i++)assert(!padded[i]);
        restart();uint32_t small_len=sizeof(data);int small_ok=0;
        assert(!server_read_fragment_with_sum(&server,&ex,902,0,0,data,&small_len,wrong,&small_ok));
        assert(small_len==sizeof(small)&&small_ok&&!memcmp(data,small,sizeof(small)));
        assert(!server_delete_fragment_if_sum(&server,&ex,902,0,0,sum));
        /* Quota follows variable-size replacements and rolls back pre-publication
         * failure. A partial result never claims a verified whole-payload digest. */
        efs_tls_chunk_gen=891;uint64_t before_var=server.nodes[0].used;
        memset(data,80,sizeof(data));efs_hash(data,sizeof(data),sum);
        assert(!server_write_fragment_with_sum_sync(&server,&ex,903,0,0,data,sizeof(data),sum));
        assert(server.nodes[0].used==before_var+sizeof(data));
        efs_hash(small,sizeof(small),sum);
        assert(!server_write_fragment_with_sum_sync(&server,&ex,903,0,0,small,sizeof(small),sum));
        assert(server.nodes[0].used==before_var+sizeof(small));
        uint32_t partial_len=8;int partial_ok=-1;
        assert(!server_read_fragment_with_sum(&server,&ex,903,0,0,data,&partial_len,wrong,&partial_ok));
        assert(partial_len==8&&partial_ok==EFS_STORE_SUM_PRESENT&&!memcmp(data,small,8));
        fault=1;memset(data,81,sizeof(data));efs_hash(data,sizeof(data),wrong);
        assert(server_write_fragment_with_sum_sync(&server,&ex,903,0,0,data,sizeof(data),wrong)==EFS_ERR_IO);fault=0;
        assert(server.nodes[0].used==before_var+sizeof(small));
        uint32_t all_len=sizeof(data);int all_ok=-1;
        assert(!server_read_fragment_with_sum(&server,&ex,903,0,0,data,&all_len,wrong,&all_ok));
        assert(all_len==sizeof(small)&&all_ok==EFS_STORE_SUM_VERIFIED&&!memcmp(data,small,sizeof(small)));
        /* A backend-computed digest without a supplied checksum is not trusted. */
        assert(!server_write_fragment_with_sum_sync(&server,&ex,903,0,0,small,sizeof(small),NULL));
        all_len=sizeof(data);all_ok=-1;
        assert(!server_read_fragment_with_sum(&server,&ex,903,0,0,data,&all_len,wrong,&all_ok));assert(all_ok==EFS_STORE_SUM_NONE);
        assert(!server_delete_fragment_if_sum(&server,&ex,903,0,0,sum));assert(server.nodes[0].used==before_var);
        /* Both map and referenced extent headers fail startup closed when damaged. */
        efs_tls_chunk_gen=101;efs_tls_write_root=0;char path[8192];server_fragment_path(&server,&ex,42,0,0,path,sizeof(path));
        struct mapping m;struct target *t=target_at(&server,path);assert(!mapping_read(path,t,&m));
        for(int header=0;header<=1;header++){
            char body[8192];if(header)segpath(t,m.seg,body);else strcpy(body,path);
            off_t off=header?(off_t)m.off*PAGE:0;int fd=open(body,O_RDWR);assert(fd>=0);uint8_t byte;assert(pread(fd,&byte,1,off)==1);
            uint8_t bad=byte^1;assert(pwrite(fd,&bad,1,off)==1);assert(!fsync(fd));close(fd);
            server_storage_engine_close(&server);assert(server_storage_engine_init(&server)==EFS_ERR_IO);
            fd=open(body,O_RDWR);assert(fd>=0&&pwrite(fd,&byte,1,off)==1);assert(!fsync(fd));close(fd);assert(!server_storage_engine_init(&server));t=target_at(&server,path);
        }
        /* Payload corruption is rejected even with an intact mapping/header. */
        char body[8192];segpath(t,m.seg,body);int fd=open(body,O_RDWR);assert(fd>=0);off_t off=(off_t)m.off*PAGE+PAGE;
        uint8_t v;assert(pread(fd,&v,1,off)==1);uint8_t bad=v^1;assert(pwrite(fd,&bad,1,off)==1);close(fd);read_value(42,0,EFS_ERR_IO,0);
        fd=open(body,O_RDWR);assert(fd>=0&&pwrite(fd,&v,1,off)==1);close(fd);read_value(42,0,EFS_OK,19);
        server_storage_engine_close(&server);
        /* Reject nested roots in either order before ANY recovery/punching.
         * An unreferenced sentinel extent proves the first root was untouched. */
        char nested[8192],sentinel[8192];snprintf(nested,sizeof(nested),"%s/nested",server.storage_paths[0]);assert(!mkdir(nested,0755));
        snprintf(sentinel,sizeof(sentinel),"%s/.efs-containers/%016x.dat",server.storage_paths[0],63);
        int sentinel_fd=open(sentinel,O_RDWR|O_CREAT|O_EXCL,0600);assert(sentinel_fd>=0);
        assert(!ftruncate(sentinel_fd,SEG_BYTES));assert(pwrite(sentinel_fd,"untouched",9,0)==9);assert(!fsync(sentinel_fd));close(sentinel_fd);
        for(unsigned order=0;order<2;order++) {
            struct efsd_server *bad=calloc(1,sizeof(*bad));assert(bad);bad->storage_engine=1;bad->storage_path_count=2;
            strcpy(bad->storage_paths[order],server.storage_paths[0]);strcpy(bad->storage_paths[1-order],nested);
            assert(server_storage_engine_init(bad)==EFS_ERR_IO);free(bad);
            sentinel_fd=open(sentinel,O_RDONLY);char saved[9];assert(sentinel_fd>=0);
            assert(pread(sentinel_fd,saved,9,0)==9&&!memcmp(saved,"untouched",9));close(sentinel_fd);
        }
        assert(!unlink(sentinel)&&!rmdir(nested));assert(!server_storage_engine_init(&server));read_value(42,0,EFS_OK,19);
        server_storage_engine_close(&server);server.storage_engine=0;assert(server_storage_engine_init(&server)==EFS_ERR_IO);
        pthread_mutex_destroy(&server.lock);assert(!nftw(base,remove_owned,32,FTW_DEPTH|FTW_PHYS));
    }
    char legacy[]="/tmp/efs-legacy-XXXXXX";assert(mkdtemp(legacy));
    char data_dir[8192],file[8192];snprintf(data_dir,sizeof(data_dir),"%s/data",legacy);assert(!mkdir(data_dir,0755));
    snprintf(file,sizeof(file),"%s/legacy-fragment",data_dir);int fd=open(file,O_WRONLY|O_CREAT,0600);assert(fd>=0);close(fd);
    memset(&server,0,sizeof(server));server.storage_path_count=1;server.storage_engine=1;strcpy(server.storage_paths[0],legacy);
    assert(server_storage_engine_init(&server)==EFS_ERR_IO);server.storage_engine=0;assert(!server_storage_engine_init(&server));
    server_storage_engine_close(&server);server.storage_engine=1;assert(server_storage_engine_init(&server)==EFS_ERR_IO);
    assert(!nftw(legacy,remove_owned,32,FTW_DEPTH|FTW_PHYS));
    puts("containers: buffered/direct, multi-root, quota, same-range concurrency, conditional/inode GC, restart, crash boundaries and corruption PASS");
    return 0;
}
