/* New wire CREATE ownership on a fresh LOCAL disposable directory. */
#include "../../src/client/client_internal.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include "efs/meta_apply.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static char create_name[64]="pending";
static efs_ino_t create(unsigned port,efs_ino_t parent,const struct efs_opid_req *q)
{
    uint8_t buf[sizeof(struct efs_msg_inode_create)+EFS_OPID_WIRE_LEN]={0};
    struct efs_msg_inode_create *r=(void *)buf;
    r->export_id=1;r->parent=parent;r->mode=S_IFREG|0600;
    r->flags=EFS_CREATE_F_PENDING;strcpy(r->name,create_name);
    efs_opid_req_pack(q,buf+sizeof(*r));
    for(unsigned i=0;i<240;i++) {
        int fd=efs_connect_tcp("127.0.0.1",port+i%4);
        if(fd<0){usleep(10000);continue;}
        assert(!efs_set_recv_timeout(fd,5000)&&!efs_set_send_timeout(fd,5000));
        int rc=efs_send_msg(fd,EFS_MSG_INODE_CREATE,buf,sizeof(buf));
        uint8_t type=0;void *body=NULL;uint32_t len=0;
        if(!rc)rc=efs_recv_msg(fd,&type,&body,&len);
        close(fd);
        if(rc){free(body);usleep(10000);continue;}
        assert(type==EFS_MSG_INODE_CREATE_REPLY&&len==sizeof(struct efs_msg_inode_reply));
        struct efs_msg_inode_reply reply;memcpy(&reply,body,len);free(body);
        if(reply.status==EFS_INODE_RPC_BUSY||reply.status==EFS_INODE_RPC_NOT_PRIMARY){usleep(10000);continue;}
        if(reply.status!=EFS_INODE_RPC_OK)fprintf(stderr,"pending create status=%u primary=%u\n",reply.status,reply.primary_id);
        assert(reply.status==EFS_INODE_RPC_OK&&reply.inode.ino);return reply.inode.ino;
    }
    assert(!"pending CREATE retry budget");return 0;
}
int main(int argc,char **argv)
{
    assert(argc==3 || argc==4 || argc==5);unsigned port=atoi(argv[1]);efs_ino_t parent=strtoull(argv[2],NULL,10);
    int hashed=argc>=4 && strstr(argv[3],"hashed")!=NULL;
    uint32_t shard=efs_kv_inode_shard(parent);
    if(hashed) {
        for(unsigned i=0;;i++) {
            snprintf(create_name,sizeof(create_name),"pending%u",i);
            shard=efs_kv_dentry_shard(parent,create_name,EFS_META_LAYOUT_HASHED);
            if(efs_raft_shard_group(shard)!=efs_raft_shard_group(efs_kv_inode_shard(parent)))break;
            assert(i<10000);
        }
    }
    g_client.export_id=1;g_client.node_count=4;
    for(unsigned i=0;i<4;i++){g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=port+i;}
    efs_client_conn_init();
    struct efs_opid_req q={.id={.session_epoch=7,.seq=10}};
    assert(!efs_client_publication_id(10,&q.id));q.id.session_epoch=7;
    if(argc==5) {
        assert((!strcmp(argv[3],"resume") || !strcmp(argv[3],"resume-hashed") || !strcmp(argv[3],"resume-adopt") || !strcmp(argv[3],"resume-adopt-hashed")) && strlen(argv[4])==32);
        for(unsigned i=0;i<16;i++){unsigned n;assert(sscanf(argv[4]+2*i,"%2x",&n)==1);q.id.client_uuid[i]=(uint8_t)n;}
    }
    assert(!efs_client_rpc_publication_session(shard,&q.id));
    efs_ino_t ino=create(port,parent,&q);assert(create(port,parent,&q)==ino);
    if(argc==4 && (!strcmp(argv[3],"create") || !strcmp(argv[3],"create-hashed"))) {
        printf("pending_uuid=");for(unsigned i=0;i<16;i++)printf("%02x",q.id.client_uuid[i]);
        printf(" ino=%llu name=%s shard=%u hashed=%d\n",(unsigned long long)ino,create_name,shard,hashed);return 0;
    }
    uint64_t gen=0;uint32_t count=1;
    assert(!efs_client_rpc_getchunks_fileid(1,ino,0,0,NULL,&count,&gen)&&gen);
    assert(shard==efs_kv_inode_shard(ino));
    struct efs_opid_req edge=q;edge.id.seq=11;edge.ack=0;uint64_t known=0;
    assert(!efs_client_rpc_lease_edge(ino,gen,&edge,EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLEAR_PENDING,10,0,&known)&&known==1);
    if(argc==4) {
        assert(!strcmp(argv[3],"adopt") || !strcmp(argv[3],"adopt-hashed"));
        /* Exit before handing any descriptor back. The harness kills/restarts
         * the daemons, then resumes this exact edge after its token is gone. */
        printf("pending_uuid=");for(unsigned i=0;i<16;i++)printf("%02x",q.id.client_uuid[i]);
        printf(" ino=%llu name=%s shard=%u hashed=%d combined_seq=11 known=1\n",(unsigned long long)ino,create_name,shard,hashed);
        return 0;
    }
    edge.id.seq=12;edge.ack=11;
    assert(!efs_client_rpc_lease_edge(ino,gen,&edge,EFS_LEASE_EDGE_CLOSE,0,0,&known)&&known==1);
    assert(create(port,parent,&q)==ino); /* replay must not reinstall pending owner */
    edge.id.seq=13;edge.ack=12;
    int rc=EFS_ERR_BUSY;
    for(unsigned i=0;i<120 && rc==EFS_ERR_BUSY;i++) {
        rc=efs_client_rpc_lease_edge(ino,gen,&edge,EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLEAR_PENDING,10,0,&known);
        if(rc==EFS_ERR_BUSY)usleep(10000);
    }
    if(rc==EFS_ERR_STALE && !known)
        rc=efs_client_rpc_lease_edge(ino,gen,&edge,EFS_LEASE_EDGE_OPEN|EFS_LEASE_EDGE_CLEAR_PENDING,10,1,&known);
    if(rc!=EFS_ERR_STALE || known!=1)fprintf(stderr,"re-adoption rc=%d known=%llu\n",rc,(unsigned long long)known);
    assert(rc==EFS_ERR_STALE&&known==1);
    printf("pending CREATE epoch7: same inode on retry, exact adoption/CLOSE and replay cannot reacquire pending ownership PASS ino=%llu\n",(unsigned long long)ino);
    return 0;
}
