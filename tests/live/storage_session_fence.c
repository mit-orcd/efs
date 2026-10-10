/* Inspect actual daemon-sealed certificates on each private fixture member. */
#include "../../src/server/server_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv)
{
    if(argc!=7 || strlen(argv[1])!=32)return 2;
    uint8_t uuid[16];for(unsigned i=0;i<16;i++){
        unsigned n;char hex[3]={argv[1][2*i],argv[1][2*i+1],0};
        if(sscanf(hex,"%x",&n)!=1 || n>255)return 2;
        uuid[i]=(uint8_t)n;
    }
    uint32_t floor=(uint32_t)strtoul(argv[2],NULL,10);if(floor<2)return 2;
    for(unsigned i=0;i<4;i++){
        struct efsd_server server={.storage_path_count=1};
        if(strlen(argv[3+i])>=sizeof(server.storage_paths[0]))return 2;
        strcpy(server.storage_paths[0],argv[3+i]);struct efs_put_storage_guard guard={0};
        int rc=server_put_session_begin(&server,uuid,floor-1,&guard);
        if(rc!=EFS_ERR_STALE){server_put_session_end(&guard);fprintf(stderr,"member=%u old epoch unexpectedly rc=%d\n",i+1,rc);return 1;}
        rc=server_put_session_begin(&server,uuid,floor,&guard);
        if(rc!=EFS_OK){fprintf(stderr,"member=%u current epoch rc=%d\n",i+1,rc);return 1;}
        server_put_session_end(&guard);
    }
    printf("four-member storage certificates: uuid=%s reject_below=%u old PUT guards refused, current guards allowed PASS\n",argv[1],floor);return 0;
}
