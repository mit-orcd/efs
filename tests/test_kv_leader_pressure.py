#!/usr/bin/env python3
"""W166: publication's L0 refusal must also fence leadership/admission.

Execute the production predicate and admission code against independent KV
and Raft states. L0-full with free RAM used to strand flushes on a leader
that saw no pressure. The live K22 fixture covers the real handoff.
An optional source argument permits an unchanged-source negative control.
"""
from pathlib import Path
import os, re, shlex, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'src/server/raft_host.c'
s = source.read_text()
assert 'static int host_kv_pressure(' in s, 'missing shared KV leadership/admission predicate'
assert 'intkv_over=host_kv_pressure(h);' in re.sub(r'\s+', '', s), 'pump must consume the same pressure'
code = r'''
#include "efs/common.h"
#include "efs/meta_cmd.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
struct efs_kv { int room, hot; };
struct efs_raft { int role; uint64_t lag, cap; };
struct efs_raft_host { struct efs_kv *kv; uint64_t admit_busy; };
static int efs_kv_lsm_apply_room(struct efs_kv *kv, int admit) { assert(admit == 1); return kv ? kv->room : 0; }
static int efs_kv_lsm_l0_hot(struct efs_kv *kv) { return kv && kv->hot; }
int efs_raft_role(const struct efs_raft *r) { return r->role; }
int efs_raft_log_progress(struct efs_raft *r, struct efs_raft_log_progress *p) {
 *p=(struct efs_raft_log_progress){.unapplied=r->lag,.max_unapplied=r->cap};return 0;
}
''' + s[s.index('static int host_kv_pressure('):s.index('/* Set while this thread serves a command another member forwarded')] + r'''
int main(void) {
 struct efs_kv kv={0};struct efs_raft_host h={.kv=&kv};
 struct efs_raft r={.role=EFS_RAFT_LEADER,.cap=1000};
 uint8_t create[]={EFS_MD_CMD_CREATE,0};
 assert(!host_kv_pressure(&h) && !host_admit(&h,&r,create,2));
 kv.hot=1; /* Full L0, plenty of RAM, no unapplied backlog. */
 assert(host_kv_pressure(&h) && host_admit(&h,&r,create,2)==EFS_ERR_BUSY);
 uint8_t cleanup[]={EFS_MD_CMD_DECIDE,EFS_MD_CMD_RESOLVE,EFS_MD_CMD_DROP};
 for(unsigned i=0;i<sizeof(cleanup);i++) assert(!host_admit(&h,&r,&cleanup[i],1));
 uint8_t batch[]={EFS_MD_CMD_TXN_BATCH,EFS_MD_CMD_RESOLVE};
 assert(!host_admit(&h,&r,batch,2));
 kv.hot=0;assert(!host_kv_pressure(&h) && !host_admit(&h,&r,create,2));
 kv.room=EFS_ERR_BUSY;assert(host_kv_pressure(&h));
 assert(host_admit(&h,&r,create,2)==EFS_ERR_BUSY);
 kv.room=0;r.lag=749;assert(!host_admit(&h,&r,create,2));
 r.lag=750;assert(host_admit(&h,&r,create,2)==EFS_ERR_BUSY);
 r.role=EFS_RAFT_FOLLOWER;assert(!host_admit(&h,&r,create,2));
 h.kv=NULL;assert(!host_kv_pressure(&h));
 puts("KV leadership pressure: L0/RAM, clear, cleanup reserve, log watermark PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-kv-pressure-') as d:
    p = Path(d) / 'test.c'
    p.write_text(code)
    exe = p.with_suffix('')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'include'),
                    str(p), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
