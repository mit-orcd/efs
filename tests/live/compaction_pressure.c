/* Fault-build fixture only: large values through real Raft and production KV.
 * Dummy keys use the private test namespace; no user metadata opcode changes. */
#include "efs/network.h"
#include "efs/protocol.h"
#include "efs/kv_lsm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#define BODY (1024u*1024u)
static void be64(uint8_t*p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-8*i));}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char**argv)
{
    assert(argc==5);
    if(!strcmp(argv[1],"write")) {
        unsigned port=(unsigned)strtoul(argv[2],NULL,10),first=(unsigned)strtoul(argv[3],NULL,10),n=(unsigned)strtoul(argv[4],NULL,10);
        uint8_t *body=malloc(BODY+10);assert(body);body[0]=0;body[1]=0xfe;
        for(unsigned i=first;i<first+n;i++) {
            be64(body+2,i%16);memset(body+10,(i%251)+1,BODY);be64(body+10,i);
            double start=now();unsigned retries=0;
            for(;;) {
                int fd=efs_connect_tcp("127.0.0.1",port);assert(fd>=0);
                assert(!efs_set_recv_timeout(fd,5000) && !efs_set_send_timeout(fd,5000));
                assert(!efs_send_msg(fd,EFS_MSG_RAFT_MKFS,body,BODY+10));
                uint8_t type;void*reply=NULL;uint32_t len;
                int rc=efs_recv_msg(fd,&type,&reply,&len);close(fd);
                assert(!rc && type==EFS_MSG_RAFT_MKFS_REPLY && len==sizeof(struct efs_msg_raft_mkfs_reply));
                struct efs_msg_raft_mkfs_reply out;memcpy(&out,reply,len);free(reply);
                if(out.rc==EFS_OK) {
                    printf("ack seq=%u index=%llu latency_us=%llu retries=%u\n",i,(unsigned long long)out.index,(unsigned long long)((now()-start)*1e6),retries);fflush(stdout);break;
                }
                assert(out.rc==EFS_ERR_BUSY && now()-start<10);retries++;usleep(10000);
            }
        }
        free(body);return 0;
    }
    assert(!strcmp(argv[1],"verify"));
    struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_SYNC};
    struct efs_kv *kv=efs_kv_lsm_open(argv[2],&cfg);assert(kv);
    unsigned first=(unsigned)strtoul(argv[3],NULL,10),n=(unsigned)strtoul(argv[4],NULL,10);
    uint8_t*got=malloc(BODY),*want=malloc(BODY);assert(got&&want);
    for(unsigned k=0;k<16;k++) {
        unsigned last=first+n-1;while(last%16!=k)last--;
        assert(last>=first);
        uint8_t key[12]={0x0f,0xff,0xfe,0};be64(key+4,k);
        uint32_t len=BODY;memset(want,(last%251)+1,BODY);be64(want,last);
        assert(!efs_kv_get(kv,key,sizeof(key),got,&len) && len==BODY && !memcmp(got,want,BODY));
    }
    free(got);free(want);efs_kv_lsm_close(kv);
    puts("all 16 exact acknowledged pressure values survive cold KV reopen PASS");return 0;
}
