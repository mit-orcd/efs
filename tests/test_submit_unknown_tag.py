#!/usr/bin/env python3
"""W140: a proposal whose tag this build cannot apply is refused before the log.

The applied-command table in include/efs/meta_cmd.h (EFS_MD_CMD_APPLIED /
efs_md_cmd_applicable) is the one source of truth. By source shape:
host_apply_dispatch compares cmd[0] against exactly the tags in that table
and checks the predicate first; host_tag_refused is called at the top of
host_propose_impl, the only function that appends to a Raft log, and
host_bg_propose / server_raft_host_pack_propose end there too. Compiled:
the predicate over all 256 tag values, then the production gate in front
of a real single-voter Raft log: an unknown tag answers EFS_ERR_PROTO,
the log and the applied count do not move, a following CREATE commits at
the next index, and the refusal line is rate-limited to one per second.
Not covered here: the submit RPC's branches and forwarding, which all end
in host_propose_impl (source shape only); a cluster run is W140's
remaining gate.
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
header = (root/'include/efs/meta_cmd.h').read_text()
s = (root/'src/server/raft_host.c').read_text()

def function(head):
    a = s.index(head)
    while s.find(';', a) < s.find('{', a):
        a = s.index(head, a + 1)
    return s[a:s.index('\n}', a) + 2]

# The table: every entry is a defined log command, none reserved or retired.
defined = {n: int(v) for n, v in re.findall(r'^#define EFS_MD_(CMD_\w+|REQ_\w+)\s+(\d+)', header, re.M)}
table = re.search(r'#define EFS_MD_CMD_APPLIED\(X\) \\\n((?:.*\\\n)*.*)\n', header).group(1)
applied = re.findall(r'X\((\w+)\)', table)
assert applied and len(applied) == len(set(applied)), applied
values = {}
for name in applied:
    assert 'CMD_' + name in defined, ('table names an undefined command', name)
    v = defined['CMD_' + name]
    assert not (45 <= v <= 51 or 55 <= v <= 57 or v in (17, 20, 21, 22, 33, 34)), ('reserved tag in table', name, v)
    values[v] = name

# The dispatcher decides on exactly the table's tags, predicate first, halt last.
dispatch = function('static int host_apply_dispatch(')
assert dispatch.split('{', 1)[1].lstrip().startswith(
    '/* W140'), 'dispatcher must check efs_md_cmd_applicable before any handler'
assert 'if (!efs_md_cmd_applicable(cmd[0]))\n        return EFS_ERR_UNSUPPORTED;' in dispatch
assert dispatch.rstrip().endswith('return EFS_ERR_UNSUPPORTED;\n}'), dispatch[-200:]
handled = set(re.findall(r'cmd\[0\]\s*==\s*EFS_MD_CMD_(\w+)', dispatch))
assert handled == set(applied), ('dispatcher and EFS_MD_CMD_APPLIED differ',
                                 sorted(handled - set(applied)), sorted(set(applied) - handled))

# Every log append sits behind the gate.
propose = function('static int host_propose_impl(')
assert 'if (host_tag_refused(group, cmd, clen))\n        return EFS_ERR_PROTO;' in propose
assert propose.index('host_tag_refused(') < propose.index('pthread_mutex_lock('), 'gate before any state'
for call in ('efs_raft_propose(', 'efs_raft_propose_local('):
    assert s.count(call) == propose.count(call), ('log append outside host_propose_impl', call)
assert 'host_propose_impl(' in function('static int host_bg_propose(')
assert 'host_bg_propose(' in function('int server_raft_host_pack_propose(')
gate = function('static int host_tag_refused(')
assert 'efs_md_cmd_applicable(cmd[0])' in gate

predicate = r'''
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    static const int applied[256]={%s};
    for(int t=0;t<256;t++)assert(efs_md_cmd_applicable((uint8_t)t)==applied[t]);
    assert(!efs_md_cmd_applicable(0) && !efs_md_cmd_applicable(200) && !efs_md_cmd_applicable(255));
    assert(!efs_md_cmd_applicable(EFS_MD_CMD_CFG)); /* host-only, never logged */
    for(int t=45;t<=51;t++)assert(!efs_md_cmd_applicable((uint8_t)t));
    for(int t=55;t<=57;t++)assert(!efs_md_cmd_applicable((uint8_t)t));
    static const int retired[]={17,20,21,22,33,34};
    for(unsigned i=0;i<6;i++)assert(!efs_md_cmd_applicable((uint8_t)retired[i]));
    %s
    puts("applicable predicate: exactly the applied table, every request/reserved/retired/unknown tag refused PASS");
    return 0;
}
''' % (','.join('[%d]=1' % v for v in sorted(values)),
       '\n    '.join('assert(!efs_md_cmd_applicable(EFS_MD_%s));' % n for n in sorted(defined) if n.startswith('REQ_')))

log = r'''
#include "efs/raft.h"
#include "efs/common.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t clock_us=5000000;
static uint64_t now_us_(void){return clock_us;}
''' + gate + r'''
static int applied_n;static uint8_t last_tag;
static int apply_cmd(void *app,uint64_t index,uint64_t term,const uint8_t *cmd,uint32_t clen){
    (void)app;(void)index;(void)term;if(clen)last_tag=cmd[0];applied_n++;return EFS_OK;}
static int discard_send(void *net,const struct efs_raft_msg *m){(void)net;(void)m;return EFS_OK;}
/* host_propose_impl's order: the gate, then the only log append. */
static int propose(struct efs_raft *r,const uint8_t *cmd,uint32_t clen,uint64_t *idx){
    if(host_tag_refused(0,cmd,clen))return EFS_ERR_PROTO;
    return efs_raft_propose(r,cmd,clen,idx);
}
static uint64_t last_index(struct efs_raft_store *st){uint64_t i=0,t=0;assert(st->last(st,&i,&t)==EFS_OK);return i;}
int main(void){
    struct efs_raft_store *st=efs_raft_mem_create();assert(st);
    struct efs_raft_cfg cfg={0};
    cfg.id=0;cfg.n=1;cfg.voters=1;cfg.store=st;cfg.store_ctx=st;cfg.send=discard_send;
    cfg.apply=apply_cmd;cfg.election_ticks=2;cfg.heartbeat_ticks=1;
    struct efs_raft *r=efs_raft_new(&cfg);assert(r);
    for(int i=0;i<8;i++)efs_raft_tick(r);
    assert(efs_raft_role(r)==EFS_RAFT_LEADER);
    uint8_t create[40]={EFS_MD_CMD_CREATE},unknown[6]={200,1,2,3,4,5};
    uint64_t idx=0;
    assert(propose(r,create,sizeof create,&idx)==EFS_OK && idx);
    uint64_t before=last_index(st);int applied_before=applied_n;
    assert(efs_raft_commit(r)==before && efs_raft_applied(r)==before);
    /* The W140 vehicle (tag 200), a reserved, a retired, an RPC-only and
     * the host-only CFG tag: refused, nothing appended, nothing applied. */
    const uint8_t tags[]={200,45,17,EFS_MD_REQ_QUERY_POLICY,EFS_MD_CMD_CFG};
    for(unsigned i=0;i<sizeof tags;i++){
        unknown[0]=tags[i];uint64_t got=77;
        assert(propose(r,unknown,sizeof unknown,&got)==EFS_ERR_PROTO);
        assert(got==77 && last_index(st)==before && efs_raft_commit(r)==before &&
               efs_raft_applied(r)==before && applied_n==applied_before);
    }
    /* A Raft no-op (no bytes) is never refused by the gate. */
    assert(!host_tag_refused(0,NULL,0));
    /* The next valid proposal commits at the very next index. */
    assert(propose(r,create,sizeof create,&idx)==EFS_OK && idx==before+1);
    assert(efs_raft_applied(r)>=idx && applied_n==applied_before+1 && last_tag==EFS_MD_CMD_CREATE);
    /* Rate limit: one line per second, the count of suppressed refusals follows. */
    unknown[0]=200;uint64_t got=0;
    clock_us+=1000000;assert(propose(r,unknown,sizeof unknown,&got)==EFS_ERR_PROTO);
    clock_us+=999999;assert(propose(r,unknown,sizeof unknown,&got)==EFS_ERR_PROTO);
    assert(last_index(st)==before+1);
    efs_raft_free(r);efs_raft_mem_free(st);
    puts("submit unknown tag: refused with PROTO before the log, log and applied unchanged, next CREATE commits PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-submit-unknown-tag-') as d:
    d = Path(d); cc = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-D_GNU_SOURCE', '-I'+str(root/'include'), '-I'+str(root/'src/common')]
    (d/'predicate.c').write_text(predicate)
    subprocess.run(cc+flags+[str(d/'predicate.c'), '-o', str(d/'predicate')], check=True)
    subprocess.run([str(d/'predicate')], check=True, timeout=30)
    (d/'log.c').write_text(log)
    subprocess.run(cc+flags+[str(d/'log.c'), str(root/'libefs.a'), '-pthread', '-lm', '-ldl', '-o', str(d/'log')], check=True)
    run = subprocess.run([str(d/'log')], check=True, timeout=60, capture_output=True, text=True)
    print(run.stdout, end='')
    lines = [l for l in run.stderr.splitlines() if 'refused proposal tag=' in l]
    assert len(lines) == 2, run.stderr
    assert 'group=0 refused proposal tag=200 len=6' in lines[0] and lines[0].endswith('(suppressed=0)'), lines
    assert 'tag=200' in lines[1] and lines[1].endswith('(suppressed=4)'), lines
print('submit unknown tag: dispatcher and EFS_MD_CMD_APPLIED agree on %d tags, every log append behind host_tag_refused, refusal logged once per second PASS' % len(applied))
