/* AN measurement helper: one real GETATTR RPC at 10 Hz. Never writes data.
 * The FUSE mount remains the writer path; this avoids confusing kernel
 * attribute-cache hits with RPC latency. Startup finishes before READY. */
#include "../src/client/client_internal.h"
#include "../src/common/parse_host_port.h"
#include "efs/kv_key.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000ull+t.tv_nsec/1000;
}

int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    char *end;double seconds=strtod(argv[2],&end);
    if(*end || seconds<=0 || seconds>3600)return 2;
    char host[64];uint16_t port;
    if(efs_parse_host_port(argv[1],host,sizeof(host),&port))return 2;
    const char *nodes[EFS_MAX_NODES]={argv[1]};
    pthread_mutex_init(&g_client.lock,NULL);
    efs_client_init_nodes(&g_client,nodes,1);
    strcpy(g_client.export_name,"default");g_client.export_id=1;
    efs_export_init(&g_client.export,1,"default");
    g_client.export.root.shard_bits=EFS_KV_SHARD_BITS;
    g_client.export.root.shard_count=1u<<EFS_KV_SHARD_BITS;
    g_client.export.chunk_size=EFS_DEFAULT_CHUNK_SIZE;
    efs_client_setup_mount_token();
    if(efs_client_discover_nodes(&g_client,host,port) ||
       efs_client_view_mount()!=EFS_OK)return 1;
    puts("READY");fflush(stdout);
    if(getchar()!='\n')return 2;
    uint64_t next=us(),finish=next+(uint64_t)(seconds*1000000);
    unsigned missed=0,n=0;int failed=0;
    while(us()<finish){
        struct timespec t={(time_t)(next/1000000),(long)(next%1000000)*1000};
        int sleep_rc;
        do {sleep_rc=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL);} while(sleep_rc==EINTR);
        if(sleep_rc)return 1;
        uint64_t start=us();
        if(start>=finish)break;
        struct efs_inode row;
        int rc=efs_client_rpc_getattr(1,EFS_ROOT_INO,&row);
        uint64_t elapsed=us()-start;
        printf("an-stat t_us=%llu latency_us=%llu rc=%d\n",
               (unsigned long long)start,(unsigned long long)elapsed,rc);
        fflush(stdout);n++;if(rc!=EFS_OK)failed=1;
        next+=100000;
        while(next<us()){next+=100000;missed++;}
    }
    printf("an-stat-total n=%u missed=%u\n",n,missed);
    return failed || !n;
}
