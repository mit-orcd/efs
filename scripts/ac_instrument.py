#!/usr/bin/env python3
"""Instrument an isolated exported measurement source, never a live product tree.

Both comparison sources get exactly the same probe. Histograms are bounded,
clock overhead is included in both, and nothing is emitted on the hot path.
No new environment reader or runtime behavior switch is added to EFS.
"""
import argparse
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('source',type=Path)
a=p.parse_args();root=a.source.resolve()
assert '/efs-task-ac' in str(root) and (root/'src/common/rdma.c').exists(),root
probe=Path(__file__).with_name('ac_probe.h').read_text()
def edit(name,scope,fn):
    path=root/name;s=path.read_text();assert 'ac-probe:' not in s
    s='#define AC_SCOPE "'+scope+'"\n'+probe+'\n'+s
    path.write_text(fn(s))
def rdma(s):
    anchor='static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;'
    assert s.count(anchor)==1
    helper='''
static void ac_reg_lock(unsigned k) {
    uint64_t t=ac_now(); pthread_mutex_lock(&g_reg_lock); ac_record(k,ac_now()-t);
}
'''
    # Per-call-site classification; the real mutex and unlock are unchanged.
    s=s.replace('pthread_mutex_lock(&g_reg_lock);','ac_reg_lock(2);')
    s=s.replace(anchor,anchor+helper)
    start=s.index('static void harvest_recv_wcs(');end=s.index('/* Drain the completion channel.',start)
    part=s[start:end].replace('ac_reg_lock(2);','ac_reg_lock(0);')
    part=part.replace('{\n','{\n    uint64_t ac_start=ac_now();\n',1)
    pos=part.rfind('}')
    part=part[:pos]+'    ac_record(3,ac_now()-ac_start);\n'+part[pos:]
    s=s[:start]+part+s[end:]
    start=s.index('void efs_rdma_totals(');end=s.index('void efs_rdma_message_stats',start)
    s=s[:start]+s[start:end].replace('ac_reg_lock(2);','ac_reg_lock(1);')+s[end:]
    return s
def pump(s):
    start=s.index('static void *host_pump(void *arg)\n{');end=s.index('static uint64_t make_salt',start)
    part=s[start:end]
    anchor='        struct pollfd pfd;';assert part.count(anchor)==1
    part=part.replace(anchor,'        uint64_t ac_cycle=ac_now();\n'+anchor)
    anchor='        memset(&pfd, 0, sizeof(pfd));';assert part.count(anchor)==1
    part=part.replace(anchor,'        ac_record(4,ac_now()-ac_cycle);\n'+anchor)
    return s[:start]+part+s[end:]
def io(s):
    anchor='    efs_iostats_snapshot(&r);' if '    efs_iostats_snapshot(&r);' in s else '    snapshot_io(&r);'
    assert s.count(anchor)==1
    s=s.replace(anchor,'    uint64_t ac_start=ac_now();\n'+anchor)
    pos=s.rfind('}')
    return s[:pos]+'    ac_record(5,ac_now()-ac_start);\n'+s[pos:]
edit('src/common/rdma.c','rdma',rdma)
edit('src/server/raft_host.c','pump',pump)
edit('src/server/iostats.c','iostats',io)
print('AC isolated measurement overlay applied; identical instrumentation required for both variants')
