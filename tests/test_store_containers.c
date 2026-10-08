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
static int container_sync(int fd)
{
    struct stat st; assert(!fstat(fd,&st));
    char p[64],name[8192];snprintf(p,sizeof(p),"/proc/self/fd/%d",fd);
    ssize_t n=readlink(p,name,sizeof(name)-1);assert(n>=0);name[n]=0;
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
#define fsync container_sync
#define fallocate container_punch
#define rename container_rename
#include "../src/server/store_containers.c"
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
    if(!rc){assert(ok&&len==sizeof(data));uint8_t h[32];efs_hash(data,len,h);assert(!memcmp(sum,h,32));
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
        write_value(42,0,18,EFS_OK);assert(server.nodes[0].used==EFS_FRAGMENT_SIZE);
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
