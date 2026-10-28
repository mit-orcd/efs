#!/usr/bin/env python3
"""K7/R3: DECIDE and TXN_RETIRE log entries decode exactly or halt the group.

Compiles the production host_apply (with its incompatible-group state,
apply scope and applied-index cursor) and the production apply_txn_cmd
from src/server/raft_host.c over libefs.a and a real on-disk KV LSM. The
dispatch stand-in forwards transaction tags to apply_txn_cmd, which is all
host_apply_dispatch does for them.

Checked, through host_apply:
  * valid DECIDE and TXN_RETIRE apply and advance the cursor;
  * a valid DECIDE with the opposite verdict is a transaction conflict
    (PROTO): the log advances, the first verdict stands, the group stays
    compatible: distinct from a malformed encoding;
  * every truncated length of both commands, appended bytes (one byte, a
    whole extra shard, a full extra word), decision values other than
    COMMIT/ABORT, participant counts 0 and EFS_TXN_MAX_PART+1, an
    out-of-range or repeated participant shard and an out-of-range
    coordinator are UNSUPPORTED: the group is marked incompatible and the
    whole store (decision rows, the reclaim cursor/floor, the applied-index
    cursor) is byte-identical, also after a restart. One malformed RETIRE
    would otherwise reclaim an expired tombstone; it stays.
Not covered: a running Raft group or cluster (NOTRUN).
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'src/server/raft_host.c').read_text()

def function(head):
    a = s.index(head)
    while s.find(';', a) < s.find('{', a):
        a = s.index(head, a + 1)
    return s[a:s.index('\n}', a) + 2]

def define(name):
    return re.search(r'^#define %s\b.*$' % name, s, re.M).group(0) + '\n'

code = '#define EFS_FAULTS 0\n#define HOST_APPLY_RC_RING 16\n#define HOST_APPLY_RC_MASK 15\n' + \
    ''.join(define(n) for n in ('HOST_APPEND_RSV_LEN', 'HOST_LOCK_LEN', 'HOST_RETIRE_LEN')) + r'''
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include "efs/meta_cmd.h"
#include "efs/txn.h"
#include "efs/meta_apply.h" /* W77: RESOLVE/RETIRE apply through the publication stages */
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
    int damaged_row;uint32_t damaged_key_len;uint8_t damaged_key[EFS_KV_KEY_MAX];
    uint64_t incompat_index,incompat_term;uint32_t incompat_len;uint8_t incompat_tag,incompat_sub;
    struct efs_raft *r;struct efs_raft_host *host;struct efs_arc_slot arc[HOST_APPLY_RC_RING];
};
struct efs_raft_host {struct efs_kv *kv;int snap_batch;uint64_t apply_extra,obs_apply_cnt,obs_apply_cycle_us,apply_progress;};
static int write_prof_on(void){return 0;}
static uint64_t wprof_now(void){return 0;}
static void wprof_add(int k,uint64_t v){(void)k;(void)v;}
static int raft_obs_on(void){return 0;}
static uint64_t now_us_(void){return 0;}
static int efs_raft_role(struct efs_raft *r){return r->role;}
static void efs_raft_allow_campaign(struct efs_raft *r,int on){r->campaign=on;}
static void efs_raft_step_down(struct efs_raft *r){r->stepped++;}
static void host_mark_incomplete(struct host_group *g){g->kv_incomplete=1;}
static void host_clear_incomplete(struct host_group *g){g->kv_incomplete=0;}
static void host_rec_mark(struct efs_raft_host *h,uint32_t shard){(void)h;(void)shard;}
''' + function('static void wr64be(') + function('static uint64_t rd64be(') + \
    function('static void wr32be(') + function('static uint32_t rd32be(') + \
    function('static int apply_txn_cmd(') + r'''
static int host_apply_dispatch(struct efs_raft_host *h,uint64_t index,uint64_t term,uint8_t group,
                               const uint8_t *cmd,uint32_t clen){
    (void)index;(void)term;(void)group;
    assert(cmd[0]==EFS_MD_CMD_DECIDE || cmd[0]==EFS_MD_CMD_TXN_RETIRE);
    return apply_txn_cmd(h,cmd,clen);
}
''' + function('static void host_kv_cursor(') + '\n' + function('static void host_mark_incompatible(') + '\n' \
    + function('static int host_apply(void *app') + r'''
/* The whole store, keys and values. */
struct dump {uint8_t *b;size_t n,cap;};
static int dump_cb(void *u,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl){
    struct dump *d=u;
    if(d->n+8+kl+vl>d->cap){d->cap=(d->cap+8+kl+vl)*2;d->b=realloc(d->b,d->cap);assert(d->b);}
    memcpy(d->b+d->n,&kl,4);memcpy(d->b+d->n+4,&vl,4);memcpy(d->b+d->n+8,k,kl);memcpy(d->b+d->n+8+kl,v,vl);
    d->n+=8+kl+vl;return 0;
}
static struct dump snap(struct efs_kv *kv){struct dump d={0};assert(efs_kv_scan(kv,dump_cb,&d)==EFS_OK);return d;}
static int same(struct dump a,struct dump b){return a.n==b.n && !memcmp(a.b,b.b,a.n);}
static int cursor_at(struct efs_kv *kv,uint8_t group,uint64_t index){
    uint8_t key[7],want[16],v[16];uint32_t vl=sizeof(v);host_kv_cursor(group,index,key,want);
    return efs_kv_get(kv,key,7,v,&vl)==EFS_OK && vl==16 && !memcmp(v,want,16);
}
#define SEC 1000000000ull
static void mint(struct efs_txid *t,uint64_t born,uint64_t seq){memcpy(t->bytes,&born,8);memcpy(t->bytes+8,&seq,8);}
static uint32_t decide(uint8_t *out,const struct efs_txid *t,uint32_t coord,int dec,const struct efs_txn_parts *p){
    uint32_t n=23;out[0]=EFS_MD_CMD_DECIDE;memcpy(out+1,t->bytes,16);wr32be(out+17,coord);
    out[21]=(uint8_t)dec;out[22]=(uint8_t)p->n;
    for(uint32_t i=0;i<p->n;i++){wr32be(out+n,p->shard[i]);n+=4;}
    return n;
}
static uint32_t retire(uint8_t *out,const struct efs_txid *t,uint32_t coord,uint64_t now){
    out[0]=EFS_MD_CMD_TXN_RETIRE;memcpy(out+1,t->bytes,16);wr32be(out+17,coord);wr64be(out+21,now);
    return HOST_RETIRE_LEN;
}
static struct efs_raft_host H;
static struct efs_raft R;
static uint64_t next_index=1;
static unsigned refused;
/* A valid entry at the next index: applied, cursor advances. */
static int ok(const uint8_t *cmd,uint32_t clen){
    struct host_group g={.group=2,.r=&R,.host=&H};
    int rc=host_apply(&g,next_index,3,cmd,clen);
    assert(rc==EFS_OK && !g.incompatible && cursor_at(H.kv,2,next_index));
    next_index++;
    return rc;
}
/* A malformed entry: the group halts, nothing in the store moves. */
static void bad(const uint8_t *cmd,uint32_t clen,const char *what){
    struct host_group g={.group=2,.r=&R,.host=&H};
    struct dump before=snap(H.kv);
    int rc=host_apply(&g,next_index,3,cmd,clen);
    struct dump after=snap(H.kv);
    if(rc!=EFS_ERR_UNSUPPORTED || !g.incompatible || !same(before,after) ||
       g.incompat_index!=next_index || g.incompat_tag!=cmd[0] || cursor_at(H.kv,2,next_index)){
        fprintf(stderr,"FAIL %s: clen=%u rc=%d incompatible=%d same=%d\n",what,clen,rc,g.incompatible,same(before,after));
        exit(1);
    }
    /* Sticky: the same index is never applied by a retry either. */
    assert(host_apply(&g,next_index,3,cmd,clen)==EFS_ERR_UNSUPPORTED && same(before,snap(H.kv)));
    free(before.b);free(after.b);refused++;
}
int main(int argc,char **argv){
    (void)argc;
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4,0};
    H.kv=efs_kv_lsm_open(argv[1],&cfg);assert(H.kv);
    R.role=EFS_RAFT_LEADER;R.campaign=1;
    uint8_t cmd[64+4*(EFS_TXN_MAX_PART+2)],m[sizeof(cmd)];
    struct efs_txn_parts p={.n=2,.shard={5,9}};
    struct efs_txid a,b,c,d;uint32_t n,coord;int dec;
    uint64_t t0=1000*SEC;
    mint(&a,t0,1);mint(&b,t0,2);mint(&c,t0,3);mint(&d,t0,4);
    coord=efs_txn_coordinator(&a,&p);
    /* c's tombstone lives on the shard b's RETIRE reclaims from. */
    for(uint64_t seq=3;efs_txn_coordinator(&c,&p)!=efs_txn_coordinator(&b,&p);)mint(&c,t0,seq+=100);

    /* Valid DECIDE; then the opposite verdict is a conflict, not malformed. */
    n=decide(cmd,&a,coord,EFS_TXN_COMMIT,&p);assert(n==31);ok(cmd,n);
    assert(efs_txn_decision_get(H.kv,coord,&a,&dec)==EFS_OK && dec==EFS_TXN_COMMIT);
    n=decide(cmd,&a,coord,EFS_TXN_ABORT,&p);ok(cmd,n); /* PROTO verdict, log advances */
    assert(efs_txn_decision_get(H.kv,coord,&a,&dec)==EFS_OK && dec==EFS_TXN_COMMIT);
    assert(apply_txn_cmd(&H,cmd,n)==EFS_ERR_PROTO);
    /* An expired tombstone (c, retired long ago) a later RETIRE would reclaim. */
    n=decide(cmd,&c,efs_txn_coordinator(&c,&p),EFS_TXN_ABORT,&p);ok(cmd,n);
    n=retire(cmd,&c,efs_txn_coordinator(&c,&p),t0);ok(cmd,n);
    n=decide(cmd,&b,efs_txn_coordinator(&b,&p),EFS_TXN_COMMIT,&p);ok(cmd,n);

    /* DECIDE: every truncated length, appended bytes, invalid fields. */
    n=decide(cmd,&d,efs_txn_coordinator(&d,&p),EFS_TXN_COMMIT,&p);
    for(uint32_t l=1;l<n;l++)bad(cmd,l,"decide truncated");
    memcpy(m,cmd,n);m[n]=0;bad(m,n+1,"decide +1 byte");
    memcpy(m,cmd,n);wr32be(m+n,7);bad(m,n+4,"decide + a shard the count does not cover");
    memcpy(m,cmd,n);memset(m+n,0,8);bad(m,n+8,"decide +8 bytes");
    for(unsigned v=0;v<256;v++){
        if(v==EFS_TXN_COMMIT || v==EFS_TXN_ABORT)continue;
        memcpy(m,cmd,n);m[21]=(uint8_t)v;bad(m,n,"decide value");
    }
    {   /* Count 0 (exact 23-byte frame) and MAX+1 (frame matching it). */
        struct efs_txn_parts big={.n=EFS_TXN_MAX_PART};
        for(uint32_t i=0;i<EFS_TXN_MAX_PART;i++)big.shard[i]=i;
        memcpy(m,cmd,23);m[22]=0;bad(m,23,"decide count 0");
        uint32_t bn=decide(m,&d,0,EFS_TXN_COMMIT,&big);
        wr32be(m+bn,200);m[22]=EFS_TXN_MAX_PART+1;bad(m,bn+4,"decide count MAX+1");
        m[22]=255;bad(m,bn+4,"decide count 255 short");
    }
    {   struct efs_txn_parts q={.n=2,.shard={5,EFS_KV_SHARD_MASK+1}};
        uint32_t qn=decide(m,&d,5,EFS_TXN_COMMIT,&q);bad(m,qn,"decide shard out of range");
        q.shard[1]=0xffffffffu;qn=decide(m,&d,5,EFS_TXN_COMMIT,&q);bad(m,qn,"decide shard ~0");
        q.shard[1]=5;qn=decide(m,&d,5,EFS_TXN_COMMIT,&q);bad(m,qn,"decide repeated shard");
        q.shard[1]=9;qn=decide(m,&d,EFS_KV_SHARD_MASK+1,EFS_TXN_COMMIT,&q);bad(m,qn,"decide coordinator out of range");
    }

    /* TXN_RETIRE: a valid one (of b) at a clock past c's retention would
     * also reclaim c's tombstone. Malformed variants must not. */
    uint32_t live,retired,cc=efs_txn_coordinator(&c,&p),bc=efs_txn_coordinator(&b,&p);
    n=retire(cmd,&b,bc,t0+3600*SEC);
    for(uint32_t l=1;l<n;l++)bad(cmd,l,"retire truncated");
    memcpy(m,cmd,n);m[n]=0;bad(m,n+1,"retire +1 byte");
    memcpy(m,cmd,n);memset(m+n,0xab,8);bad(m,n+8,"retire +8 bytes");
    memcpy(m,cmd,n);wr32be(m+17,EFS_KV_SHARD_MASK+1);bad(m,n,"retire coordinator out of range");
    assert(efs_txn_decision_get(H.kv,cc,&c,&dec)==EFS_OK && dec==EFS_TXN_ABORT); /* tombstone kept */
    struct dump keep=snap(H.kv);

    /* Restart: nothing a refused entry touched became durable. */
    efs_kv_lsm_close(H.kv);H.kv=efs_kv_lsm_open(argv[1],&cfg);assert(H.kv);
    struct dump reopened=snap(H.kv);assert(same(keep,reopened));
    assert(cursor_at(H.kv,2,next_index-1) && !cursor_at(H.kv,2,next_index));
    /* The exact frame still applies: b retires, c's expired tombstone goes. */
    ok(cmd,n);
    assert(efs_txn_decision_get(H.kv,bc,&b,&dec)==EFS_OK && dec==EFS_TXN_COMMIT);
    assert(efs_txn_decision_get(H.kv,cc,&c,&dec)==EFS_ERR_STALE);
    assert(efs_txn_decision_count(H.kv,bc,&live,&retired)==EFS_OK);
    /* A RETIRE of an unknown txn is a verdict (NOT_FOUND), not malformed. */
    { struct efs_txid z;mint(&z,t0+7200*SEC,9);n=retire(cmd,&z,5,t0+7200*SEC);ok(cmd,n); }
    efs_kv_lsm_close(H.kv);
    free(keep.b);free(reopened.b);
    printf("txn apply exact: %u malformed DECIDE/TXN_RETIRE entries halted the group with the store unchanged "
           "(also after restart); valid entries, a conflicting verdict and an unknown RETIRE advance PASS\n",refused);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-txn-apply-exact-') as d:
    d = Path(d); cc = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-D_GNU_SOURCE', '-I'+str(root/'include'), '-I'+str(root/'src/common')]
    (d/'t.c').write_text(code)
    subprocess.run(cc+flags+[str(d/'t.c'), str(root/'libefs.a'), '-pthread', '-lm', '-ldl', '-o', str(d/'t')], check=True)
    subprocess.run([str(d/'t'), str(d/'kv')], check=True, timeout=120)
