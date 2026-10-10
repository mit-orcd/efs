/* Exercise the production cache boundary and partial-write error behavior. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int partial_write;
static unsigned write_calls;
static unsigned read_calls;
static ssize_t counted_pread(int fd,void *buf,size_t size,off_t offset)
{ read_calls++;return pread(fd,buf,size,offset); }
static ssize_t fault_pwrite(int fd,const void *buf,size_t size,off_t offset)
{
    write_calls++;
    if(partial_write==2){partial_write=0;errno=ENOSPC;return -1;}
    if(partial_write==1){partial_write=2;return pwrite(fd,buf,size<3?size:3,offset);}
    return pwrite(fd,buf,size,offset);
}
#define pwrite fault_pwrite
#define pread counted_pread
#include "../src/meta/query_disk.c"
#undef pwrite
#undef pread
static int failures;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);failures++;}}while(0)
int main(void)
{
    struct efs_query_disk *d=calloc(1,sizeof(*d));if(!d)return 1;
    strcpy(d->path,"/tmp/efs-query-window-XXXXXX");
    if(!mkdtemp(d->path)){free(d);return 1;}
    pthread_mutex_init(&d->mutex,NULL);
    for(int i=0;i<FD_CACHE;i++)d->fds[i].fd=-1;
    uint8_t data[READ_WINDOW*3+17],got[READ_WINDOW*2],change[31];
    for(size_t i=0;i<sizeof(data);i++)data[i]=(uint8_t)(i*17+i/251);
    memset(change,0xa7,sizeof(change));
    int fd=make_file(d,0,'r');CHECK(fd>=0);
    if(fd>=0){CHECK(!io_at(fd,data,sizeof(data),0,1));close(fd);}
    const uint64_t offsets[]={0,READ_WINDOW-7,READ_WINDOW+11,sizeof(data)-17};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++){
        CHECK(!disk_io(d,0,'r',got,17,offsets[i],0));CHECK(!memcmp(got,data+offsets[i],17));
    }
    CHECK(!disk_io(d,0,'r',got,sizeof(got),13,0));CHECK(!memcmp(got,data+13,sizeof(got)));
    CHECK(disk_io(d,0,'r',got,18,sizeof(data)-17,0)==EFS_ERR_IO);
    CHECK(disk_io(d,0,'r',got,2,INT64_MAX,0)==EFS_ERR_OVERFLOW);
    /* Warm a window, write inside it, then write across its end. */
    CHECK(!disk_io(d,0,'r',got,17,20,0));
    CHECK(!disk_io(d,0,'r',change,sizeof(change),23,1));memcpy(data+23,change,sizeof(change));
    CHECK(!disk_io(d,0,'r',got,17,20,0));CHECK(!memcmp(got,data+20,17));
    CHECK(!disk_io(d,0,'r',change,sizeof(change),READ_WINDOW-9,1));memcpy(data+READ_WINDOW-9,change,sizeof(change));
    CHECK(!disk_io(d,0,'r',got,17,READ_WINDOW-17,0));CHECK(!memcmp(got,data+READ_WINDOW-17,17));
    /* The failed write changed three disk bytes: cached old bytes must vanish. */
    CHECK(!disk_io(d,0,'r',got,17,100,0));partial_write=1;
    CHECK(disk_io(d,0,'r',change,10,100,1)==EFS_ERR_IO);memcpy(data+100,change,3);
    CHECK(!disk_io(d,0,'r',got,17,100,0));CHECK(!memcmp(got,data+100,17));
    /* Exact median lookup needs one cold window; gaps and end misses must
     * still report NOT_FOUND rather than returning a neighboring inode. */
    struct disk_node nodes[31]={0},found;uint64_t at=UINT64_MAX;
    for(unsigned i=0;i<31;i++)nodes[i].row.ino=(uint64_t)(i+1)*8192;
    d->part[0].inodes=31;fd=make_file(d,0,'d');CHECK(fd>=0);
    if(fd>=0){CHECK(!io_at(fd,nodes,sizeof(nodes),0,1));close(fd);}
    read_calls=0;CHECK(!locate(d,16*8192,&found,&at));
    CHECK(found.row.ino==16*8192 && at==15 && read_calls==1);
    CHECK(locate(d,0,&found,NULL)==EFS_ERR_NOT_FOUND);
    CHECK(locate(d,3*4096,&found,NULL)==EFS_ERR_NOT_FOUND);
    CHECK(locate(d,32*8192,&found,NULL)==EFS_ERR_NOT_FOUND);
    /* Raw record assembly must preserve bytes and never add an index entry
     * after a short write followed by ENOSPC. It uses two writes, not three. */
    uint8_t key[11]={0,99,EFS_KV_KIND_INODE},value[37],record[48];
    memset(value,0x39,sizeof(value));d->disk_limit=UINT64_MAX;
    struct writer writer={.d=d,.shard=99,.raw=-1,.index=-1};
    write_calls=0;CHECK(!save(&writer,key,sizeof(key),value,sizeof(value)));
    CHECK(write_calls==2 && writer.count==1 && writer.offset==sizeof(record));
    CHECK(!io_at(writer.raw,record,sizeof(record),0,0));
    CHECK(!memcmp(record,key,sizeof(key)) && !memcmp(record+sizeof(key),value,sizeof(value)));
    partial_write=1;CHECK(save(&writer,key,sizeof(key),value,sizeof(value))==EFS_ERR_IO);
    CHECK(writer.count==1 && writer.offset==sizeof(record));
    struct stat index_stat;CHECK(!fstat(writer.index,&index_stat));
    CHECK(index_stat.st_size==(off_t)sizeof(struct slot));
    CHECK(!save(&writer,key,sizeof(key),NULL,0));CHECK(writer.count==2);
    close(writer.raw);close(writer.index);
    /* Reusing every FD slot must not reuse bytes from its previous file. */
    for(uint32_t shard=1;shard<=FD_CACHE+2;shard++){
        uint8_t byte=(uint8_t)shard;fd=make_file(d,shard,'r');CHECK(fd>=0);
        if(fd>=0){CHECK(!io_at(fd,&byte,1,0,1));close(fd);}
        CHECK(!disk_io(d,shard,'r',got,1,0,0));CHECK(got[0]==byte);
    }
    CHECK(!disk_io(d,0,'r',got,17,100,0));CHECK(!memcmp(got,data+100,17));
    efs_query_disk_free(d);
    printf("query disk I/O: windows, lookup, packed records, partial ENOSPC and FD reuse %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
