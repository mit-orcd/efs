/* Actual worker; peer transport/authority adapters. Real hashes and receipts.
 * Physical/quorum acceptance belongs to tests/faults/harness.py. */
#include "../src/server/server_internal.h"
#include "efs/repair_job.h"
#include "efs/checksum.h"
#include "efs/network.h"
#include "efs/put_upload.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static struct efs_repair_job canonical;
static uint8_t data[3][65536];
static uint8_t request[70000],kind;
static unsigned admits,records,seals,retires,mode,connects,destroys;
static struct efs_conn connection;
struct efs_conn *server_peer_conn_new(const char *host,uint16_t port){(void)host;(void)port;connects++;return &connection;}
void efs_conn_destroy(struct efs_conn *c){assert(c==&connection);destroys++;}
int efs_conn_send_msg(struct efs_conn *c,uint8_t type,const void *body,uint32_t len)
{(void)c;assert(len<=sizeof(request));memcpy(request,body,len);kind=type;return 0;}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **out,uint32_t *len)
{
    (void)c;*out=NULL;*len=0;*type=kind+1;
    if(kind==EFS_MSG_REPAIR_STORE_ID){
        *len=28+EFS_BUILD_ID_LEN;uint8_t *b=calloc(1,*len);*out=b;
        efs_put_upload_wr32(b+4,1);efs_put_upload_wr32(b+8,canonical.nodes[canonical.fi]);
        memcpy(b+12,canonical.target_store,16);memcpy(b+28,EFS_BUILD_ID,strlen(EFS_BUILD_ID));return 0;
    }
    if(kind==EFS_MSG_GET_CHUNK){
        struct efs_msg_get_chunk q;memcpy(&q,request,sizeof(q));
        unsigned f=q.fragment_index;assert(f<3 && f!=canonical.fi);
        *len=1+32+65536;uint8_t *b=calloc(1,*len);*out=b;
        memcpy(b+1,canonical.sums[f],32);memcpy(b+33,data[f],65536);
        if(mode==1)b[33]^=1; /* server advertises the correct digest for wrong bytes */
        if(mode==2)*len-=1;
        return 0;
    }
    assert(kind==EFS_MSG_REPAIR_TARGET);
    unsigned seal=request[4+EFS_BUILD_ID_LEN]==2;
    if(mode==3 && !seal)return -1;
    struct efs_repair_job j;assert(!efs_repair_job_decode(request+4+EFS_BUILD_ID_LEN+5,EFS_REPAIR_JOB_BYTES,&j));
    if(!seal)assert(!memcmp(request+4+EFS_BUILD_ID_LEN+5+EFS_REPAIR_JOB_BYTES,data[j.fi],65536));
    *len=4+EFS_REPAIR_RECEIPT_BYTES;uint8_t *b=calloc(1,*len);*out=b;
    assert(!efs_repair_receipt_encode(&j,seal?EFS_REPAIR_RECEIPT_SEAL:EFS_REPAIR_RECEIPT_DATA,b+4));
    if(mode==4 && !seal)b[4+12]^=1; /* receipt belongs to another context */
    return 0;
}
int server_raft_host_repair_prepare(const struct efs_repair_job *q,struct efs_repair_job *j,uint8_t *state,uint8_t *flags)
{(void)q;*j=canonical;*state=*flags=0;return 0;}
int server_raft_host_repair_record(unsigned op,const struct efs_repair_job *j,uint32_t arg,const uint8_t *receipt,uint32_t len)
{
    (void)arg;
    if(op==EFS_REPAIR_OP_ADMIT){admits++;return 0;}
    if(op==EFS_REPAIR_OP_RETIRE){retires++;return 0;}
    int rc=efs_repair_receipt_verify(j,op==EFS_REPAIR_OP_DATA?EFS_REPAIR_RECEIPT_DATA:EFS_REPAIR_RECEIPT_SEAL,receipt,len);
    if(rc)return rc;
    if(op==EFS_REPAIR_OP_DATA){records++;if(mode==5)return EFS_ERR_IO;}
    else seals++;
    return 0;
}
int main(void)
{
    struct efsd_server *s=calloc(1,sizeof(*s));assert(s);pthread_mutex_init(&s->lock,NULL);
    s->storage_path_count=1;s->repair_experiment=1;s->node_count=3;
    for(unsigned f=0;f<3;f++){s->nodes[f].id=f+1;s->nodes[f].port=20000+f;strcpy(s->nodes[f].addr,"127.0.0.1");}
    canonical=(struct efs_repair_job){.ino=9,.inode_gen=10,.object=11,.fragment_len=65536,
        .owner=1,.epoch=1,.state=EFS_REPAIR_ADMITTED,.nodes={1,2,3}};
    canonical.uuid[0]=1;canonical.target_store[0]=2;
    for(unsigned i=0;i<65536;i++){data[0][i]=(uint8_t)(i*19);data[1][i]=(uint8_t)(i*7+3);data[2][i]=data[0][i]^data[1][i];}
    for(unsigned f=0;f<3;f++)efs_hash(data[f],65536,canonical.sums[f]);
    assert(server_repair_run(s,NULL)==EFS_ERR_INVAL);
    assert(server_repair_run(NULL,&canonical)==EFS_ERR_INVAL);
    s->repair_experiment=0;assert(server_repair_run(s,&canonical)==EFS_ERR_ACCES && !admits);s->repair_experiment=1;
    for(unsigned fi=0;fi<3;fi++)for(mode=0;mode<6;mode++){
        canonical.fi=fi;admits=records=seals=retires=connects=destroys=0;
        int rc=server_repair_run(s,&canonical);assert(connects==destroys);
        if(mode==0){assert(!rc && admits==1 && records==1 && seals==1 && retires==1);}
        else if(mode<3){assert(rc==EFS_ERR_CHECKSUM && !admits && !records && !seals && !retires);}
        else {assert(rc && admits==1 && !seals && !retires);}
    }
    pthread_mutex_destroy(&s->lock);free(s);
    puts("actual repair worker: all roles, default refusal, poisoned/truncated source before admission, lost target reply, misbound receipt and DATA commit failure preserve debt/no terminal ACK PASS");return 0;
}
