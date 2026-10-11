#!/usr/bin/env python3
"""Actual FUSE query callback retains publisher hints while reauthorizing every RPC."""
import os,re,shlex,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_QUERY_FUSE_SOURCE',str(root/'src/client/efs_fuse.c'))).read_text()
a=s.index('#define EFS_FIND_RES_N');b=s.index('/* Fetch an authorized',a);state=s[a:b]
a=s.index('static int find_query_ensure(');b=s.index('\n/* Result length',a);function=s[a:b]
code=r'''
#include "efs/common.h"
#include "efs/protocol.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct fuse_ctx {unsigned uid,gid;};
static struct fuse_ctx caller={1001,1001};
static const struct fuse_ctx *ll_ctx(void){return &caller;}
static unsigned calls,hints[8],credentials;
static int denied;
static uint64_t stats_now_ms(void){return 1000;}
static int efs_command_path_parse_v4(const char *p,struct efs_msg_query_snapshot_v4 *q){(void)p;memset(q,0,sizeof(*q));q->base.version=EFS_QUERY_PAGE_VERSION;return 0;}
static int query_credentials(struct efs_msg_query_snapshot *q){q->uid=caller.uid;q->gid=caller.gid;credentials++;return denied?EFS_ERR_ACCES:0;}
static uint64_t efs_client_rpc_deadline_ms(void){return 0;}
static void efs_client_rpc_set_deadline_ms(uint64_t n){(void)n;}
static int efs_rc_to_errno(int rc){return rc;}
static int fake_rpc(efs_node_id_t hint,struct efs_msg_query_snapshot_reply*r,char**text){
 hints[calls]=hint;efs_node_id_t owner=hint?hint:(calls?2:3);calls++;
 memset(r,0,sizeof(*r));r->primary_id=owner;*text=strdup(owner==3?"owner-three":"owner-two");assert(*text);r->text_len=strlen(*text);return 0;
}
static int efs_client_rpc_query_snapshot_v4_at(const struct efs_msg_query_snapshot_v4*q,efs_node_id_t hint,struct efs_msg_query_snapshot_reply*r,char**t){(void)q;return fake_rpc(hint,r,t);}
static int efs_client_rpc_query_snapshot_v4(const struct efs_msg_query_snapshot_v4*q,struct efs_msg_query_snapshot_reply*r,char**t) __attribute__((unused));
static int efs_client_rpc_query_snapshot_v4(const struct efs_msg_query_snapshot_v4*q,struct efs_msg_query_snapshot_reply*r,char**t){(void)q;return fake_rpc(0,r,t);}
'''+state+function+r'''
int main(void){
 assert(!find_query_ensure(42,"unused","request-nonce-1"));
 assert(!find_query_ensure(42,"unused","request-nonce-1"));
 assert(calls==2 && hints[0]==0 && hints[1]==3 && credentials==2);
 assert(!strcmp(g_find_res[0].text,"owner-three"));
 assert(!find_query_ensure(42,"unused","request-nonce-2"));assert(hints[2]==0);
 caller.uid=1002;assert(!find_query_ensure(42,"unused","request-nonce-1"));assert(hints[3]==0);
 denied=1;assert(find_query_ensure(42,"unused","request-nonce-1")==EFS_ERR_ACCES);
 assert(calls==4 && credentials==5 && g_find_work==0); /* no bytes-cache authorization bypass */
 puts("query callback affinity: same nonce keeps owner, new nonce/caller starts independently, every callback reauthorizes PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-query-callback-') as td:
 p=Path(td)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root/'include'),str(p),'-lpthread','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
