#!/usr/bin/env python3
"""Compatibility caching is bounded, exact-roster and never caches a refusal."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/pack_write.c').read_text()
code=s[:s.index('static uint64_t pack_now(')]+r'''
#include <assert.h>
#include <stdio.h>
struct efs_client g_client;
static uint64_t now=1;static unsigned probes;static int bad,network_fail;
static uint64_t pack_now(void){return now;}
struct efs_conn *efs_client_conn_get(efs_node_id_t n){return (struct efs_conn *)(uintptr_t)n;}
void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){assert((uintptr_t)c==n);}
void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){assert((uintptr_t)c==n);}
int efs_conn_send_msg(struct efs_conn *c,uint8_t type,const void *p,uint32_t n){assert(c&&type==EFS_MSG_PACK_CAPABILITIES&&!p&&!n);probes++;return network_fail;}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **p,uint32_t *n){assert(c);*type=EFS_MSG_PACK_CAPABILITIES_REPLY;*n=4;*p=calloc(1,4);assert(*p);efs_put_upload_wr32(*p,bad?0:1);return 0;}
'''+s[s.index('static int pack_capabilities('):s.index('static int pack_progress(')]+r'''
int main(void){
 g_client.node_count=4;for(unsigned i=0;i<4;i++)g_client.nodes[i].id=i+1;
 uint32_t members;assert(!pack_capabilities(&members)&&members==15&&probes==4);
 now+=PACK_CAPABILITY_NS-1;assert(!pack_capabilities(&members)&&probes==4);
 now++;bad=1;assert(pack_capabilities(&members)==EFS_ERR_PROTO&&probes==5);
 assert(pack_capabilities(&members)==EFS_ERR_PROTO&&probes==6); /* refusal is not cached */
 bad=0;network_fail=1;assert(pack_capabilities(&members)==EFS_ERR_NET&&probes==7);
 network_fail=0;assert(!pack_capabilities(&members)&&members==15&&probes==11);
 g_client.nodes[3].id=3;assert(pack_capabilities(&members)==EFS_ERR_PROTO&&probes==11);
 g_client.nodes[3].id=4;now=0;assert(!pack_capabilities(&members)&&probes==15); /* backward clock reprobes */
 g_client.node_count=3;assert(!pack_capabilities(&members)&&members==7&&probes==18);
 puts("mounted pack compatibility: exact roster, bounded positive cache, expiry/backward clock, mixed-version/network refusal and no negative caching PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-cap-') as d:
 p=Path(d)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
