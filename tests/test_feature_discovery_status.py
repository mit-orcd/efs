#!/usr/bin/env python3
"""Actual feature reply distinguishes unavailable authority from disabled policy."""
from pathlib import Path
import os,subprocess,tempfile,shlex
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_FEATURE_HANDLER_SOURCE',str(root/'src/server/handler.c'))).read_text()
a=s.index('case EFS_MSG_GET_FEATURES: {');b=s.index('case EFS_MSG_QUERY_SNAPSHOT:',a)
handler=s[a:b]
s=(root/'src/server/raft_host.c').read_text();a=s.index('int server_raft_host_query_features_checked(');b=s.index('\n}',a)+2
helper=s[a:b]
code=r'''
#include "efs/protocol.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct host { int query_enabled,query_started; } host={1,1},*g_host=&host;
static int policy_rc;static uint32_t policy_mask=3;
static int host_query_policy_authority(uint32_t *mask){*mask=policy_mask;return policy_rc;}
'''+helper+r'''
uint32_t server_raft_host_query_features(void) { uint32_t mask=0;return server_raft_host_query_features_checked(&mask) ? 0 : mask; }
static struct efs_msg_features_reply captured;
int efs_conn_send_msg(struct efs_conn *conn,uint8_t type,const void *data,uint32_t len) {
 (void)conn;assert(type==EFS_MSG_GET_FEATURES_REPLY&&len==sizeof(captured));memcpy(&captured,data,len);return 0;
}
static void reply(void) {struct efs_conn *conn=NULL;switch(EFS_MSG_GET_FEATURES) {
'''+handler+r'''
 default:assert(0);
}}
int main(void){
 uint32_t mask=99;assert(server_raft_host_query_features_checked(NULL)==EFS_ERR_INVAL);
 policy_rc=EFS_ERR_BUSY;reply();assert(captured.status!=EFS_FEATURES_OK && !captured.features);
 assert(server_raft_host_query_features_checked(&mask)==EFS_ERR_BUSY);
 policy_rc=0;policy_mask=3;reply();assert(captured.status==EFS_FEATURES_OK&&captured.features==3);
 policy_mask=0;reply();assert(captured.status==EFS_FEATURES_OK&&!captured.features);
 host.query_started=0;reply();assert(captured.status!=EFS_FEATURES_OK);
 host.query_enabled=0;reply();assert(captured.status==EFS_FEATURES_OK&&!captured.features);
 g_host=NULL;reply();assert(captured.status!=EFS_FEATURES_OK);
 puts("feature discovery: transient authority unavailable; genuine disabled mask remains authoritative; startup ceiling and reply status PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-feature-status-') as td:
 p=Path(td)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
