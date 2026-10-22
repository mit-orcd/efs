/* Actual pending CREATE identity survives a discarded reply on private stores. */
#include "../../src/client/client_internal.h"
#include "efs/session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void lease_complete(struct efs_client_lease_request *r)
{
    for(unsigned i=0;i<20&&!r->complete;i++) {
        int rc=efs_client_lease_resume(r,0);
        assert(rc==EFS_OK || rc==EFS_ERR_BUSY || rc==EFS_ERR_NET);
        if(!r->complete)usleep(10000);
    }
    assert(r->complete&&r->result==EFS_OK);
}
int main(int argc,char **argv)
{
    assert(argc==5);unsigned port=atoi(argv[1]);efs_ino_t parent=strtoull(argv[2],0,10);
    int discard=atoi(argv[4]);assert(discard==0||discard==1);
    g_client.export_id=1;g_client.node_count=4;
    for(unsigned i=0;i<4;i++){g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=port+i;}
    efs_client_conn_init();
    struct efs_client_create_request r={0};
    assert(!efs_client_create_begin(&r,parent,argv[3],S_IFREG|0600,getuid(),getgid()));
    struct efs_opid id=r.wire.q.id;
    assert(efs_client_create_resume(&r,1)==EFS_ERR_BUSY&&r.wire.routed&&!r.attempted);
    assert(efs_client_rpc_lookup(1,parent,argv[3],NULL)==EFS_ERR_NOT_FOUND);
    if(discard) {
        assert(!efs_client_rpc_publication_session(r.wire.shard,&r.wire.q.id));
        r.admitted=1;r.attempted=1;
        struct efs_msg_create_retained request=r.wire;request.probe=0;
        int fd=efs_connect_declared("127.0.0.1",port);assert(fd>=0);
        assert(!efs_send_msg(fd,EFS_MSG_CREATE_RETAINED,&request,sizeof(request)));
        /* Discard the reply, then recover through production read-only proof
         * or identical resubmission if the first send never reached apply. */
        close(fd);
    }
    for(unsigned i=0;i<20&&!r.complete;i++) {
        int rc=efs_client_create_resume(&r,0);
        assert(rc==EFS_OK || rc==EFS_ERR_BUSY || rc==EFS_ERR_NET);
        if(!r.complete)usleep(10000);
    }
    assert(r.complete&&r.result==EFS_OK&&r.inode.ino&&r.generation&&r.slot==-1);
    assert(!memcmp(&id,&r.wire.q.id,sizeof(id)));
    struct efs_inode visible;assert(!efs_client_rpc_lookup(1,parent,argv[3],&visible)&&visible.ino==r.inode.ino);
    struct efs_client_lease_request edge={0};
    assert(!efs_client_lease_begin(&edge,r.inode.ino,r.generation,EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLEAR_PENDING,id.seq));
    lease_complete(&edge);assert(!efs_client_lease_reset(&edge));
    assert(!efs_client_lease_begin(&edge,r.inode.ino,r.generation,EFS_LEASE_EDGE_CLOSE,0));
    lease_complete(&edge);assert(!efs_client_lease_reset(&edge));
    assert(!efs_client_create_resume(&r,0)&&r.inode.ino==visible.ino); /* saved result only */
    struct efs_client_create_request negative={0};
    assert(!efs_client_create_begin(&negative,parent,argv[3],S_IFREG|0600,getuid(),getgid()));
    for(unsigned i=0;i<20&&!negative.complete;i++) {
        int rc=efs_client_create_resume(&negative,0);
        assert(rc==EFS_ERR_EXIST || rc==EFS_ERR_BUSY || rc==EFS_ERR_NET);
    }
    assert(negative.complete&&negative.result==EFS_ERR_EXIST&&negative.slot==-1);
    assert(!efs_client_create_reset(&negative));
    printf("retained CREATE discard=%d ino=%llu shard=%u: pure routing probe, exact result, pending adoption/CLOSE and negative receipt PASS\n",discard,(unsigned long long)r.inode.ino,r.wire.shard);
    assert(!efs_client_create_reset(&r));
}
