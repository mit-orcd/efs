/* Retained production request object on a disposable fixture inode. */
#include "../../src/client/client_internal.h"
#include "efs/session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv)
{
    assert(argc==3);
    unsigned port=(unsigned)atoi(argv[1]);
    efs_ino_t ino=strtoull(argv[2],NULL,10);
    g_client.export_id=1;g_client.node_count=4;
    for(unsigned i=0;i<4;i++) {
        g_client.nodes[i].id=i+1;
        strcpy(g_client.nodes[i].addr,"127.0.0.1");
        g_client.nodes[i].port=port+i;
    }
    efs_client_conn_init();
    uint64_t generation=0;uint32_t count=1;
    assert(efs_client_rpc_getchunks_fileid(1,ino,0,0,NULL,&count,&generation)==EFS_OK && generation);
    struct efs_client_lease_request r={0};
    uint8_t uuid[EFS_OPID_UUID_LEN];
    for(unsigned stage=0;stage<2;stage++) {
        unsigned flags=stage?EFS_LEASE_EDGE_CLOSE:EFS_LEASE_EDGE_OPEN;
        assert(efs_client_lease_begin(&r,ino,generation,flags,0)==EFS_OK);
        if(!stage)memcpy(uuid,r.q.id.client_uuid,sizeof(uuid));
        else assert(!memcmp(uuid,r.q.id.client_uuid,sizeof(uuid)));
        assert(efs_client_lease_reset(&r)==EFS_ERR_BUSY);
        assert(efs_client_lease_resume(&r,0)==EFS_OK && r.complete && r.slot==-1);
        uint64_t known=0;
        assert(efs_client_rpc_lease_edge(ino,generation,&r.q,flags,0,1,&known)==EFS_OK && known==1);
        assert(efs_client_lease_resume(&r,1)==EFS_OK);
        assert(efs_client_lease_reset(&r)==EFS_OK);
    }
    puts("actual retained sequenced client lease: authoritative FileID, full UUID, OPEN/CLOSE stored proof and safe reset PASS");
    return 0;
}
