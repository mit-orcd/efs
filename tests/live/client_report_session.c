/* REPORT mount-session fencing on disposable private live stores. */
#include "../../src/client/client_internal.h"
#include "efs/session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
struct packet { uint32_t len; uint8_t body[sizeof(struct efs_msg_report_chunks)+sizeof(struct efs_chunk_rec)+sizeof(struct efs_ino_size_rec)+EFS_REPORT_SESSION_LEN]; };
static struct efs_msg_inode_reply rpc(unsigned port,const struct packet *p,int discard)
{
    unsigned node=0;
    for(unsigned attempt=0;attempt<100;attempt++) {
        int fd=efs_connect_tcp("127.0.0.1",port+node);assert(fd>=0);
        assert(!efs_set_recv_timeout(fd,10000) && !efs_set_send_timeout(fd,10000));
        assert(!efs_send_msg(fd,EFS_MSG_REPORT_CHUNKS,p->body,p->len));
        if(discard){close(fd);return (struct efs_msg_inode_reply){0};}
        uint8_t type;void *body=NULL;uint32_t len;
        assert(!efs_recv_msg(fd,&type,&body,&len));close(fd);
        assert(type==EFS_MSG_REPORT_CHUNKS_REPLY && len==sizeof(struct efs_msg_inode_reply));
        struct efs_msg_inode_reply r;memcpy(&r,body,len);free(body);
        if (attempt < 4 || attempt == 99) fprintf(stderr,"REPORT attempt=%u status=%u primary=%u bytes=%u\n",attempt,r.status,r.primary_id,p->len);
        if(r.status==EFS_INODE_RPC_NOT_PRIMARY){node=r.primary_id?r.primary_id-1:(node+1)%4;assert(node<4);usleep(10000);continue;}
        if(r.status==EFS_INODE_RPC_BUSY){usleep(10000);continue;}
        return r;
    }
    assert(!"REPORT routing deadline");return (struct efs_msg_inode_reply){0};
}
static struct packet pack(const struct efs_chunk_rec *rec,const struct efs_opid *id)
{
    struct packet p={0};struct efs_msg_report_chunks h={.export_id=1,.count=1,.sync=EFS_REPORT_F_SYNC|EFS_REPORT_F_SESSION,.ino_count=1};
    memcpy(p.body,&h,sizeof(h));size_t used=sizeof(h);
    size_t n=efs_chunk_rec_pack(p.body+used,sizeof(p.body)-used,rec);assert(n);used+=n;
    struct efs_ino_size_rec ino={.ino=rec->ino,.size=1};memcpy(p.body+used,&ino,sizeof(ino));used+=sizeof(ino);
    efs_report_session_pack(p.body+used,id);used+=EFS_REPORT_SESSION_LEN;p.len=used;return p;
}
int main(int argc,char **argv)
{
    assert(argc==5 || argc==6);unsigned port=atoi(argv[1]);efs_ino_t ino=strtoull(argv[2],0,10);
    if(argc==6 && !strcmp(argv[4],"cold")) {
        struct packet p;FILE *f=fopen(argv[5],"rb");assert(f && fread(&p,sizeof(p),1,f)==1 && fgetc(f)==EOF);fclose(f);
        assert(p.len<=sizeof(p.body) && rpc(port,&p,0).status==EFS_INODE_RPC_STALE);
        puts("REPORT cold restart: revoked mapping replay remains STALE PASS");return 0;
    }
    g_client.export_id=1;g_client.node_count=4;
    for(unsigned i=0;i<4;i++){g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=port+i;}
    efs_client_conn_init();struct efs_chunk_rec rec;uint32_t count=1;uint64_t generation=0;
    assert(!efs_client_rpc_getchunks_fileid(1,ino,0,0,&rec,&count,&generation) && count==1 && generation);
    struct efs_opid id;assert(!efs_client_publication_id(1,&id));
    struct packet p=pack(&rec,&id);rpc(port,&p,1); /* Lost reply retains the immutable mapping body. */
    assert(rpc(port,&p,0).status==EFS_INODE_RPC_OK);
    assert(!efs_client_rpc_report_dirty_raft(1,&rec,1,NULL,0,1));
    struct packet malformed=p;malformed.len--;assert(rpc(port,&malformed,0).status==EFS_INODE_RPC_INVAL);
    malformed=p;malformed.body[sizeof(struct efs_msg_report_chunks)+efs_chunk_rec_wire_size(&rec)+sizeof(struct efs_ino_size_rec)]=0;
    memset(malformed.body+malformed.len-EFS_REPORT_SESSION_LEN,0,16);assert(rpc(port,&malformed,0).status==EFS_INODE_RPC_INVAL);
    char hex[33],address[64];for(unsigned i=0;i<16;i++)snprintf(hex+2*i,3,"%02x",id.client_uuid[i]);snprintf(address,sizeof(address),"127.0.0.1:%u",port);
    pid_t child=fork();assert(child>=0);if(!child){execl(argv[3],argv[3],"raft-session",address,"fence",hex,(char*)NULL);_exit(127);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(rpc(port,&p,0).status==EFS_INODE_RPC_STALE);
    assert(efs_client_rpc_report_dirty_raft(1,&rec,1,NULL,0,1)==EFS_ERR_STALE);
    rec.chunk_index=17;struct packet first=pack(&rec,&id);assert(rpc(port,&first,0).status==EFS_INODE_RPC_STALE);
    FILE *f=fopen(argv[4],"wb");assert(f && fwrite(&p,sizeof(p),1,f)==1 && !fclose(f));
    puts("REPORT mount identity: discarded reply/exact mapping retry, malformed frame, revoked replay and first-touch rejection PASS");
}
