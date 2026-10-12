#!/usr/bin/env python3
"""Execute actual host_apply plus public admission prefix; adapter stubs only."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
b=s.index('static int host_apply(void');apply=s[b:s.index('\n}',b)+2]
b=s.index('void server_raft_host_submit(');end=s.index('    if(clen && cmd[0]==EFS_MD_REQ_PACK_GC_VIEW)',b)
public=s[b:end]+'}\n'
code=r'''
#include "efs/meta_cmd.h"
#include "efs/metadata.h"
#include "efs/session.h"
#include "efs/meta_apply.h"
#include <assert.h>
#include <string.h>
#include <stdint.h>
#define HOST_APPLY_RC_MASK 7
#define HOST_APPEND_RSV_LEN 99
#define HOST_LOCK_LEN 99
#define HOST_SESS_CREATE_LEN 100
#define EFS_RAFT_LEADER 1
#define EFS_FAULTS 0
struct efs_raft_host {void *kv;int running,snap_batch;uint64_t apply_extra,obs_apply_cnt,obs_apply_cycle_us;};
struct host_group {struct efs_raft_host *host;int import_block;void *r;uint8_t group;uint64_t arc_idx[8],arc_term[8],arc_extra[8];int arc_rc[8];};
struct efs_msg_raft_mkfs_reply {int rc,leader_hint;};
static struct efs_raft_host *g_host;
static int verdict,cursors,scopes;
static int raft_obs_on(void){return 0;}
static uint64_t now_us_(void){return 0;}
static int efs_raft_role(void *r){(void)r;return 0;}
static void host_mark_incomplete(struct host_group *g){(void)g;}
static void host_clear_incomplete(struct host_group *g){(void)g;}
static int efs_kv_lsm_apply_begin(struct efs_kv *kv){(void)kv;scopes++;return 0;}
static int efs_kv_lsm_apply_end(struct efs_kv *kv,const struct efs_kv_item *cursor){(void)kv;if(cursor)cursors++;return 0;}
static void host_kv_cursor(uint8_t g,uint64_t i,uint8_t *k,uint8_t *v){(void)g;(void)i;memset(k,0,7);memset(v,0,16);}
static int host_apply_dispatch(struct efs_raft_host *h,uint64_t i,uint64_t t,uint8_t g,const uint8_t *c,uint32_t n){(void)h;(void)i;(void)t;(void)g;(void)c;(void)n;return verdict;}
'''+apply+public+r'''
int main(void){
 struct efs_kv kv={0};struct efs_raft_host h={.kv=&kv,.running=1};struct host_group g={.host=&h};g_host=&h;
 uint8_t cmd[]={EFS_MD_CMD_REPAIR_JOB,1};
 int logical[]={EFS_ERR_BUSY,EFS_ERR_STALE,EFS_ERR_INVAL,EFS_ERR_PROTO};
 for(unsigned j=0;j<4;j++){
  verdict=logical[j];int before=cursors;assert(host_apply(&g,10+j,1,cmd,2)==0);assert(cursors==before+1);assert(g.arc_rc[(10+j)&7]==logical[j]);
  uint8_t other[]={EFS_MD_CMD_CREATE};verdict=0;assert(!host_apply(&g,20+j,1,other,1));assert(cursors==before+2);
 }
 int storage[]={EFS_ERR_IO,EFS_ERR_NOMEM};
 for(unsigned j=0;j<2;j++){verdict=storage[j];int before=cursors;assert(host_apply(&g,30+j,1,cmd,2)==storage[j]);assert(cursors==before);assert(g.arc_rc[(30+j)&7]==storage[j]);}
 struct efs_msg_raft_mkfs_reply reply;uint8_t raw[]={0,EFS_MD_CMD_REPAIR_JOB,3};
 server_raft_host_submit(raw,sizeof(raw),&reply);assert(reply.rc==EFS_ERR_ACCES);
 assert(scopes==10);return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'host.c';p.write_text(code);b=Path(d)/'host'
 subprocess.run(['cc','-std=gnu11','-Wno-unused-variable','-I'+str(root/'include'),str(p),'-o',str(b)],check=True)
 subprocess.run([b],check=True)
print('actual host repair verdict progression, IO retry cursor, public refusal PASS')
