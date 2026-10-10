/* Retained production request object on a disposable fixture inode. */
#include "../../src/client/client_internal.h"
#include "efs/session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
int main(int argc,char **argv)
{
    assert(argc==4);
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
        /* Existing wrappers share this exact full mount UUID/epoch. Legacy
         * observation may succeed, but it cannot replace or erase the edge. */
        if(!stage)
            assert(efs_client_rpc_hold(1,ino,1,g_client.flock_token)==EFS_OK);
        else
            assert(efs_client_rpc_hold(1,ino,1,g_client.flock_token)==EFS_ERR_STALE);
        assert(efs_client_rpc_hold(1,ino,0,g_client.flock_token)==EFS_ERR_STALE);
        known=0;
        assert(efs_client_rpc_lease_edge(ino,generation,&r.q,flags,0,1,&known)==EFS_OK && known==1);
        assert(efs_client_lease_reset(&r)==EFS_OK);
    }
    struct efs_opid old={0};memcpy(old.client_uuid,uuid,sizeof(uuid));old.session_epoch=g_client.opid_epoch;old.seq=1;
    int revoked=1;
    assert(efs_client_rpc_identity_revoked(&old,&revoked)==EFS_OK && !revoked);
    char hex[33],address[64];for(unsigned i=0;i<16;i++)snprintf(hex+2*i,3,"%02x",uuid[i]);
    snprintf(address,sizeof(address),"127.0.0.1:%u",port);
    const char *fault=getenv("EFS_TEST_PUT_FENCE_FAULT_FILE");
    if(fault){
        FILE *armed=fopen(fault,"wb");assert(armed && fputc(1,armed)!=EOF && !fclose(armed));
        pid_t failing=fork();assert(failing>=0);
        if(!failing){execl(argv[3],argv[3],"raft-session",address,"fence",hex,(char *)NULL);_exit(127);}
        int failed;assert(waitpid(failing,&failed,0)==failing && WIFEXITED(failed) && WEXITSTATUS(failed));
        assert(!unlink(fault));
        /* An applied local retry alone cannot finish the global barrier. */
        int proof=EFS_ERR_BUSY;
        for(unsigned attempt=0;attempt<40;attempt++){
            proof=efs_client_rpc_identity_revoked(&old,&revoked);
            if(proof==EFS_OK)break;
            assert(proof==EFS_ERR_BUSY || proof==EFS_ERR_NET || proof==EFS_ERR_NOT_PRIMARY || proof==EFS_ERR_NO_QUORUM);usleep(100000);
        }
        assert(proof==EFS_OK && !revoked);
        puts("certificate fsync EIO: coordinator failed, global revocation remained incomplete, same barrier resumes PASS");
    }
    pid_t child=fork();assert(child>=0);
    if(!child){execl(argv[3],argv[3],"raft-session",address,"fence",hex,(char *)NULL);_exit(127);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(efs_client_rpc_identity_revoked(&old,&revoked)==EFS_OK && revoked);
    struct efs_opid future=old;future.session_epoch+=100;
    assert(efs_client_rpc_identity_revoked(&future,&revoked)==EFS_OK && !revoked);
    struct efs_opid missing=old;memset(missing.client_uuid,0xfe,16);
    assert(efs_client_rpc_identity_revoked(&missing,&revoked)==EFS_ERR_NOT_FOUND && !revoked);
    assert(efs_client_lease_begin(&r,ino,generation,EFS_LEASE_EDGE_OPEN,0)==EFS_OK);
    struct efs_opid_req revoked_request=r.q;
    int revoked_rc=efs_client_lease_resume(&r,0);
    fprintf(stderr,"revoked retained lease: rc=%d complete=%d revoked=%d slot=%d attempted=%d\n",
        revoked_rc,r.complete,r.revoked,r.slot,r.attempted);
    assert(revoked_rc==EFS_ERR_STALE && r.complete && r.revoked && r.slot==-1);
    /* Cached establishment may permit an RPC attempt, but the fresh guarded
     * edge still rejects this epoch. Neither a cached grant nor missing state
     * is revocation proof, and a retry cannot acquire a new identity. */
    assert(!memcmp(&r.q,&revoked_request,sizeof(r.q)));
    assert(efs_client_lease_resume(&r,0)==EFS_ERR_STALE);
    uint64_t known=1;
    assert(efs_client_rpc_lease_edge(ino,generation,&r.q,EFS_LEASE_EDGE_OPEN,0,1,&known)==EFS_ERR_STALE && !known);
    assert(efs_client_lease_reset(&r)==EFS_OK);
    printf("storage-session-certificate uuid=%s reject_below=%u\n",hex,old.session_epoch+1);
    puts("durably revoked epoch: authoritative global proof, future/missing rejection and terminal retained lease recovery PASS");
    puts("actual retained sequenced client lease: authoritative FileID, full UUID, OPEN/CLOSE stored proof, legacy mutation barrier and safe reset PASS");
    return 0;
}
