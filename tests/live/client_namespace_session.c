/* Actual directory mutation session admission/fencing on private stores. */
#include "../../src/client/client_internal.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
struct packet {uint8_t type;uint32_t bytes;uint8_t body[sizeof(struct efs_msg_inode_rename_at)+EFS_OPID_WIRE_LEN];};
static struct efs_msg_inode_reply rpc(unsigned port,const struct packet *p)
{
 unsigned node=0;
 for(unsigned attempt=0;attempt<100;attempt++) {
  int fd=efs_connect_tcp("127.0.0.1",port+node);assert(fd>=0);
  assert(!efs_set_recv_timeout(fd,5000) && !efs_set_send_timeout(fd,5000));
  assert(!efs_send_msg(fd,p->type,p->body,p->bytes));uint8_t type;void *body=NULL;uint32_t bytes;
  assert(!efs_recv_msg(fd,&type,&body,&bytes));close(fd);
  assert(type==p->type+1 && bytes==sizeof(struct efs_msg_inode_reply));
  struct efs_msg_inode_reply r;memcpy(&r,body,bytes);free(body);
  if(r.status==EFS_INODE_RPC_NOT_PRIMARY){node=r.primary_id?r.primary_id-1:(node+1)%4;assert(node<4);usleep(10000);continue;}
  if(r.status==EFS_INODE_RPC_BUSY){usleep(10000);continue;}
  return r;
 }
 assert(!"namespace RPC deadline");return (struct efs_msg_inode_reply){0};
}
static struct packet pack(uint8_t type,const void *body,uint32_t bytes,const struct efs_opid *id,uint64_t seq)
{
 struct packet p={.type=type,.bytes=bytes+EFS_OPID_WIRE_LEN};assert(p.bytes<=sizeof(p.body));
 memcpy(p.body,body,bytes);struct efs_opid_req q={.id=*id};q.id.seq=seq;
 efs_opid_req_pack(&q,p.body+bytes);return p;
}
int main(int argc,char **argv)
{
 assert(argc==4 || argc==5 || argc==6);unsigned port=atoi(argv[1]);efs_ino_t parent=strtoull(argv[2],0,10);
 if(argc==6 && !strcmp(argv[4],"cold")){
  assert(!strcmp(argv[4],"cold"));struct packet old[7];FILE *f=fopen(argv[5],"rb");assert(f);
  assert(fread(old,sizeof(old),1,f)==1 && fgetc(f)==EOF);fclose(f);
  for(unsigned i=0;i<7;i++){assert(old[i].bytes<=sizeof(old[i].body));struct efs_msg_inode_reply r=rpc(port,&old[i]);fprintf(stderr,"cold namespace case=%u status=%u\n",i,r.status);assert(r.status==EFS_INODE_RPC_STALE);}
  puts("namespace cold restart: all seven pre-revocation receipts remain STALE PASS");return 0;
 }
 g_client.export_id=1;g_client.node_count=4;
 for(unsigned i=0;i<4;i++){g_client.nodes[i].id=i+1;strcpy(g_client.nodes[i].addr,"127.0.0.1");g_client.nodes[i].port=port+i;}
 efs_client_conn_init();struct efs_opid id;assert(!efs_client_publication_id(1,&id));
 struct packet saved[7];
 struct efs_msg_inode_create create={.export_id=1,.parent=parent,.mode=S_IFREG|0600,.uid=getuid(),.gid=getgid()};strcpy(create.name,"a");
 saved[0]=pack(EFS_MSG_INODE_CREATE,&create,sizeof(create),&id,1);
 struct efs_msg_inode_reply r=rpc(port,&saved[0]);assert(r.status==EFS_INODE_RPC_OK && r.inode.ino);efs_ino_t ino=r.inode.ino;
 create.mode=S_IFDIR|0700;strcpy(create.name,"d");saved[1]=pack(EFS_MSG_INODE_CREATE,&create,sizeof(create),&id,2);
 assert(rpc(port,&saved[1]).status==EFS_INODE_RPC_OK);
 struct efs_msg_inode_link link={.export_id=1,.src_ino=ino,.new_parent=parent};strcpy(link.new_name,"b");
 saved[2]=pack(EFS_MSG_INODE_LINK,&link,sizeof(link),&id,3);assert(rpc(port,&saved[2]).status==EFS_INODE_RPC_OK);
 struct efs_msg_inode_rename_at rename={.export_id=1,.old_parent=parent,.new_parent=parent};strcpy(rename.old_name,"b");strcpy(rename.new_name,"c");
 saved[3]=pack(EFS_MSG_INODE_RENAME_AT,&rename,sizeof(rename),&id,4);assert(rpc(port,&saved[3]).status==EFS_INODE_RPC_OK);
 struct efs_msg_inode_unlink unlink={.export_id=1,.parent=parent};strcpy(unlink.name,"c");
 saved[4]=pack(EFS_MSG_INODE_UNLINK,&unlink,sizeof(unlink),&id,5);assert(rpc(port,&saved[4]).status==EFS_INODE_RPC_OK);
 strcpy(unlink.name,"d");unlink.is_dir=1;saved[5]=pack(EFS_MSG_INODE_UNLINK,&unlink,sizeof(unlink),&id,6);
 assert(rpc(port,&saved[5]).status==EFS_INODE_RPC_OK);
 strcpy(create.name,"still-empty");saved[6]=pack(EFS_MSG_INODE_CREATE,&create,sizeof(create),&id,7);
 assert(rpc(port,&saved[6]).status==EFS_INODE_RPC_OK);
 for(unsigned i=0;i<7;i++)assert(rpc(port,&saved[i]).status==EFS_INODE_RPC_OK);
 char hex[33],address[64];for(unsigned i=0;i<16;i++)snprintf(hex+2*i,3,"%02x",id.client_uuid[i]);snprintf(address,sizeof(address),"127.0.0.1:%u",port);
 pid_t child=fork();assert(child>=0);if(!child){execl(argv[3],argv[3],"raft-session",address,"fence",hex,(char *)NULL);_exit(127);}
 int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
 for(unsigned i=0;i<7;i++)assert(rpc(port,&saved[i]).status==EFS_INODE_RPC_STALE);
 if(argc>=5){FILE *f=fopen(argv[4],"wb");assert(f);assert(fwrite(saved,sizeof(saved),1,f)==1);assert(!fclose(f));}
 create.mode=S_IFREG|0600;strcpy(create.name,"late");struct packet late=pack(EFS_MSG_INODE_CREATE,&create,sizeof(create),&id,8);
 assert(rpc(port,&late).status==EFS_INODE_RPC_STALE);
 create.mode=S_IFDIR|0700;strcpy(create.name,"late-dir");late=pack(EFS_MSG_INODE_CREATE,&create,sizeof(create),&id,9);
 assert(rpc(port,&late).status==EFS_INODE_RPC_STALE);
 strcpy(link.new_name,"late-link");late=pack(EFS_MSG_INODE_LINK,&link,sizeof(link),&id,10);
 assert(rpc(port,&late).status==EFS_INODE_RPC_STALE);
 strcpy(rename.old_name,"a");strcpy(rename.new_name,"moved");late=pack(EFS_MSG_INODE_RENAME_AT,&rename,sizeof(rename),&id,11);
 assert(rpc(port,&late).status==EFS_INODE_RPC_STALE);
 strcpy(unlink.name,"a");unlink.is_dir=0;late=pack(EFS_MSG_INODE_UNLINK,&unlink,sizeof(unlink),&id,12);
 assert(rpc(port,&late).status==EFS_INODE_RPC_STALE);
 strcpy(unlink.name,"still-empty");unlink.is_dir=1;late=pack(EFS_MSG_INODE_UNLINK,&unlink,sizeof(unlink),&id,13);
 assert(rpc(port,&late).status==EFS_INODE_RPC_STALE);
 struct efs_inode found;assert(!efs_client_rpc_lookup(1,parent,"a",&found) && found.ino==ino);
 assert(!efs_client_rpc_lookup(1,parent,"still-empty",&found));
 for(unsigned i=0;i<4;i++)assert(efs_client_rpc_lookup(1,parent,(const char *[]){"late","late-dir","late-link","moved"}[i],NULL)==EFS_ERR_NOT_FOUND);
 if(argc==6){
  efs_ino_t untouched=strtoull(argv[5],NULL,10);assert(untouched && efs_kv_inode_shard(untouched)!=efs_kv_inode_shard(parent));
  create.parent=untouched;create.mode=S_IFREG|0600;strcpy(create.name,"late-first-touch");
  late=pack(EFS_MSG_INODE_CREATE,&create,sizeof(create),&id,14);
  assert(rpc(port,&late).status==EFS_INODE_RPC_STALE);
  assert(efs_client_rpc_lookup(1,untouched,"late-first-touch",NULL)==EFS_ERR_NOT_FOUND);
  puts("namespace first untouched shard: completed global revocation rejects old identity without a local floor PASS");
 }
 if(argc>=5){puts("namespace fenced names retained for cold restart PASS");return 0;}
 /* Independent new identity cleans only this helper's disposable namespace. */
 id.client_uuid[0]^=0x80;strcpy(unlink.name,"a");unlink.is_dir=0;late=pack(EFS_MSG_INODE_UNLINK,&unlink,sizeof(unlink),&id,1);assert(rpc(port,&late).status==EFS_INODE_RPC_OK);
 strcpy(unlink.name,"still-empty");unlink.is_dir=1;late=pack(EFS_MSG_INODE_UNLINK,&unlink,sizeof(unlink),&id,2);assert(rpc(port,&late).status==EFS_INODE_RPC_OK);
 puts("actual namespace session: CREATE/mkdir/link/rename/unlink/rmdir exact replay; completed fence rejects cached and new effects, authoritative names preserved PASS");
}
