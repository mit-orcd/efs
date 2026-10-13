/* Native/Massif retained-log probe. Run ONLY on a private copy of a stopped
 * store: opening replays/truncates torn or reserved tails. Never user data. */
#include "../../src/raft/raft_disk_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv)
{
    if(argc!=2){fprintf(stderr,"Usage: raft_heap_probe PRIVATE_LOG_COPY\n");return 2;}
    struct efs_raft_disk *d=efs_raft_disk_open(argv[1],EFS_RAFT_DISK_NOSYNC);
    if(!d)return 1;
    uint64_t entries=0,commands=0,cached=0,index=0;
    for(unsigned i=0;i<RAFT_DISK_MAX_GROUPS;i++)if(d->g[i]) {
        struct raft_disk_group *g=d->g[i];entries+=g->n;index+=(uint64_t)g->cap*sizeof(*g->log);
        for(unsigned j=0;j<g->n;j++){commands+=g->log[j].clen;if(g->log[j].cmd)cached+=g->log[j].clen;}
        printf("group=%u entries=%u command_bytes=%llu snapshot=%llu\n",i,g->n,
               (unsigned long long)g->bytes_all,(unsigned long long)g->snap_idx);
#ifdef RAFT_INDEX_RESIDENT_BYTES
        g->index_advised=0;raft_group_index_trim(g);
#endif
        uint8_t *buf=malloc(RAFT_CMD_MAX);if(!buf)return 1;
        for(unsigned j=0;j<g->n;j+=g->n/16+1) {
            uint32_t len=RAFT_CMD_MAX;uint64_t term=0;
            if(g->ops.get(g,g->log_base+1+j,&term,buf,&len)!=EFS_OK ||
               len!=g->log[j].clen || term!=g->log[j].term)return 1;
        }
        free(buf);
    }
    printf("entries=%llu durable_command_bytes=%llu cached_command_bytes=%llu index_capacity_bytes=%llu\n",
           (unsigned long long)entries,(unsigned long long)commands,(unsigned long long)cached,(unsigned long long)index);
    FILE *status=fopen("/proc/self/status","r");char line[256];
    if(status){while(fgets(line,sizeof(line),status))
        if(!strncmp(line,"VmRSS:",6)||!strncmp(line,"RssAnon:",8)||!strncmp(line,"RssFile:",8))fputs(line,stdout);
        fclose(status);}
    efs_raft_disk_close(d);return 0;
}
