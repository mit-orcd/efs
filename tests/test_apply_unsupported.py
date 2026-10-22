#!/usr/bin/env python3
"""W127-W131: an unsupported or malformed entry is never applied as success.

Compiles the production host_apply (with the incompatible-group state) over a
real on-disk KV LSM. Dispatch is a controlled stand-in that writes one row
inside the apply scope, so the test sees exactly what the scope commits.
Decoder slices (DIR, TXN) and the dispatch fall-through are checked too.
Not covered here: the Raft core retry loop and service refusal on a running
host (proposal/read gates are checked by source shape only).
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = Path(os.environ.get('EFS_RAFT_HOST_SOURCE', root/'src/server/raft_host.c')).read_text()
def function(head):
    # The definition, not a forward declaration: skip matches whose
    # signature ends in ';' before any '{'.
    a = s.index(head)
    while s.find(';', a) < s.find('{', a):
        a = s.index(head, a + 1)
    b = s.index('\n}', a) + 2
    return s[a:b]
lens = ''.join('#define %s %s\n' % (n, re.search(r'^#define %s +(\S+)' % n, s, re.M).group(1))
               for n in ('HOST_APPEND_RSV_LEN', 'HOST_LOCK_LEN', 'HOST_DIR_LEN'))
# Dispatch must end in the unsupported verdict, and reserved/retired tags too.
dispatch = function('static int host_apply_dispatch(')
assert dispatch.rstrip().endswith('return EFS_ERR_UNSUPPORTED;\n}'), dispatch[-200:]
assert 'if(EFS_MD_CMD_IS_RESERVED(cmd[0]))\n        return EFS_ERR_UNSUPPORTED;' in dispatch
# Proposals and reads of an incompatible group are refused.
assert 'if (host_group_incompatible(h, group))\n        return EFS_ERR_UNSUPPORTED;' in function('static int host_propose_impl(')
assert 'if (host_group_incompatible(h, group))\n        return EFS_ERR_UNSUPPORTED;' in function('static int host_read_index_admitted(')
code = '#define EFS_FAULTS 0\n#define HOST_APPLY_RC_RING 16\n#define HOST_APPLY_RC_MASK 15\n' + lens + r'''
#include "efs/kv.h"
#include "efs/kv_lsm.h"
#include "efs/meta_cmd.h"
#include "efs/txn.h"
#include "../src/server/apply_ring.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define EFS_RAFT_LEADER 1
#define APPLY_LOG(...) ((void)0)
#define WP_APPLY 0
struct efs_raft {int role,campaign,stepped;};
struct efs_raft_host;
struct host_group {
    uint8_t group;int import_block,kv_incomplete,incompatible;
    uint64_t incompat_index,incompat_term;uint32_t incompat_len;uint8_t incompat_tag,incompat_sub;
    struct efs_raft *r;struct efs_raft_host *host;struct efs_arc_slot arc[HOST_APPLY_RC_RING];
};
struct efs_raft_host {struct efs_kv *kv;int snap_batch;uint64_t apply_extra,obs_apply_cnt,obs_apply_cycle_us;};
static int write_prof_on(void){return 0;}
static uint64_t wprof_now(void){return 0;}
static void wprof_add(int k,uint64_t v){(void)k;(void)v;}
static int raft_obs_on(void){return 0;}
static uint64_t now_us_(void){return 0;}
static int efs_raft_role(struct efs_raft *r){return r->role;}
static void efs_raft_allow_campaign(struct efs_raft *r,int on){r->campaign=on;}
static void efs_raft_step_down(struct efs_raft *r){r->stepped++;}
static void wr64be(uint8_t *p,uint64_t v){for(int i=7;i>=0;i--){p[i]=(uint8_t)v;v>>=8;}}
static void host_mark_incomplete(struct host_group *g){g->kv_incomplete=1;}
static void host_clear_incomplete(struct host_group *g){g->kv_incomplete=0;}
static int verdict,dispatched;
static int host_apply_dispatch(struct efs_raft_host *h,uint64_t index,uint64_t term,uint8_t group,
                               const uint8_t *cmd,uint32_t clen){
    (void)term;(void)group;(void)cmd;(void)clen;dispatched++;
    uint8_t key[3]={0x10,0,(uint8_t)index},value[1]={1};
    struct efs_kv_item put={EFS_KV_PUT,key,3,value,1};
    assert(!efs_kv_batch(h->kv,&put,1)); /* a side effect inside the scope */
    return verdict;
}
''' + function('static void host_kv_cursor(') + '\n' + function('static void host_mark_incompatible(') + '\n' \
    + function('static int host_apply(void *app') + r'''
static int has(struct efs_kv *kv,const uint8_t *key,uint32_t kl){uint8_t v[32];uint32_t vl=sizeof(v);return efs_kv_get(kv,key,kl,v,&vl)==EFS_OK;}
static int row(struct efs_kv *kv,uint64_t index){uint8_t key[3]={0x10,0,(uint8_t)index};return has(kv,key,3);}
static int cursor_at(struct efs_kv *kv,uint8_t group,uint64_t index){
    uint8_t key[7],want[16],v[16];uint32_t vl=sizeof(v);host_kv_cursor(group,index,key,want);
    return efs_kv_get(kv,key,7,v,&vl)==EFS_OK && vl==16 && !memcmp(v,want,16);
}
int main(int argc,char **argv){
    (void)argc;
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};
    struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);
    struct efs_raft r={EFS_RAFT_LEADER,1,0};
    struct efs_raft_host h={.kv=kv};struct host_group g={.group=2,.r=&r,.host=&h};
    uint8_t create[64]={EFS_MD_CMD_CREATE},unknown[8]={200};
    /* An applied entry commits its row and the cursor. */
    verdict=EFS_OK;assert(host_apply(&g,1,7,create,40)==EFS_OK && row(kv,1) && cursor_at(kv,2,1));
    /* CREATE's verdicts normally become OK; an unsupported one must not. */
    verdict=EFS_ERR_UNSUPPORTED;
    assert(host_apply(&g,2,7,create,40)==EFS_ERR_UNSUPPORTED);
    assert(!row(kv,2) && cursor_at(kv,2,1)); /* scope aborted, cursor unchanged */
    assert(g.incompatible && g.incompat_index==2 && g.incompat_term==7 && g.incompat_tag==EFS_MD_CMD_CREATE);
    assert(!r.campaign && r.stepped==1);
    /* Sticky: the retried index (and any later one) is never dispatched. */
    int before=dispatched;verdict=EFS_OK;
    assert(host_apply(&g,2,7,create,40)==EFS_ERR_UNSUPPORTED && dispatched==before);
    assert(host_apply(&g,3,7,unknown,8)==EFS_ERR_UNSUPPORTED && dispatched==before && !row(kv,3));
    /* Restart: nothing of the refused entry became durable. */
    efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);
    assert(row(kv,1) && !row(kv,2) && cursor_at(kv,2,1) && !cursor_at(kv,2,2));
    /* Another group on the same KV still applies. */
    struct efs_raft r2={EFS_RAFT_LEADER,1,0};h.kv=kv;
    struct host_group g0={.group=0,.r=&r2,.host=&h};
    verdict=EFS_OK;assert(host_apply(&g0,1,3,create,40)==EFS_OK && cursor_at(kv,0,1));
    /* W131: no KV engine is refused, never consumed; a no-op still applies. */
    struct efs_raft_host none={0};struct host_group gn={.group=0,.r=&r2,.host=&none};
    before=dispatched;assert(host_apply(&gn,5,3,create,40)==EFS_ERR_IO && dispatched==before);
    assert(host_apply(&g0,2,3,create,0)==EFS_OK);
    efs_kv_lsm_close(kv);
    puts("apply unsupported: aborted scope, unchanged cursor after restart, sticky incompatible group, other group progresses, missing engine refused PASS");
    return 0;
}
'''
# Decoder slices with controlled storage verdicts.
decoders = '#define HOST_DIR_LEN %s\n' % re.search(r'^#define HOST_DIR_LEN +(\S+)', s, re.M).group(1) + r'''
#include "efs/common.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define APPLY_LOG(...) ((void)0)
struct efs_kv;struct efs_raft_host{struct efs_kv *kv;};
static uint64_t rd64be(const uint8_t *p){uint64_t v=0;for(int i=0;i<8;i++)v=v<<8|p[i];return v;}
static int dir_rc;
static int efs_meta_dir_begin_split(struct efs_kv *k,uint64_t d){(void)k;(void)d;return dir_rc;}
static int efs_meta_dir_migrate_one(struct efs_kv *k,uint64_t d){(void)k;(void)d;return dir_rc;}
static int efs_meta_dir_finish_hashed(struct efs_kv *k,uint64_t d){(void)k;(void)d;return dir_rc;}
''' + function('static int apply_dir_cmd(') + r'''
int main(void){
    struct efs_raft_host h={0};uint8_t cmd[HOST_DIR_LEN]={EFS_MD_CMD_DIR,EFS_MD_DIR_BEGIN};
    dir_rc=EFS_OK;assert(apply_dir_cmd(&h,cmd,HOST_DIR_LEN,1)==EFS_OK);
    int benign[]={EFS_ERR_NOT_FOUND,EFS_ERR_INVAL,EFS_ERR_BUSY};
    for(unsigned i=0;i<3;i++){dir_rc=benign[i];assert(apply_dir_cmd(&h,cmd,HOST_DIR_LEN,1)==EFS_OK);}
    dir_rc=EFS_ERR_IO;assert(apply_dir_cmd(&h,cmd,HOST_DIR_LEN,1)==EFS_ERR_IO);       /* W130 retry */
    dir_rc=EFS_ERR_NOMEM;assert(apply_dir_cmd(&h,cmd,HOST_DIR_LEN,1)==EFS_ERR_NOMEM);
    dir_rc=EFS_OK;
    assert(apply_dir_cmd(&h,cmd,HOST_DIR_LEN-1,1)==EFS_ERR_UNSUPPORTED);               /* W128 short */
    cmd[1]=99;assert(apply_dir_cmd(&h,cmd,HOST_DIR_LEN,1)==EFS_ERR_UNSUPPORTED);       /* unknown kind */
    puts("apply DIR: storage failures retry, short frame and unknown kind unsupported PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-apply-unsupported-') as d:
    d = Path(d); cc = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-D_GNU_SOURCE', '-I'+str(root/'include'), '-I'+str(root/'src/common')]
    (d/'apply.c').write_text(code)
    subprocess.run(cc+flags+[str(d/'apply.c'), str(root/'libefs.a'), '-pthread', '-lm', '-ldl', '-o', str(d/'apply')], check=True)
    subprocess.run([str(d/'apply'), str(d/'kv')], check=True, timeout=60)
    (d/'dir.c').write_text(decoders)
    subprocess.run(cc+flags+[str(d/'dir.c'), '-o', str(d/'dir')], check=True)
    subprocess.run([str(d/'dir')], check=True, timeout=30, stderr=subprocess.DEVNULL)
# TXN: short frames and an unknown tag are unsupported (source shape; the
# decoder returns before touching the transaction table).
txn = function('static int apply_txn_cmd(')
assert 'int rc = EFS_ERR_UNSUPPORTED' in txn and txn.count('return EFS_ERR_UNSUPPORTED;') == 1, txn
print('apply decoders: TXN short frames and unknown tags unsupported PASS')
