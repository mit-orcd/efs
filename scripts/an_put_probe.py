#!/usr/bin/env python3
"""Apply temporary AN counters to an exported measurement tree, never a checkout.

No authority, retry, timeout, routing or storage decision changes. The probe
counts accepted proof admissions separately from successful core read-round
starts. Quorum rounds include other workload/background reads: they are not
exclusive causal attribution to STATUS. RPC counters count successful frame
sends (including distinct routing retries), not logical caller invocations.
"""
import argparse
import hashlib
import json
from pathlib import Path

NAMES = ('fragment_attempts', 'fragment_ok', 'fragment_error', 'status_queries',
         'status_wire', 'owner_wire', 'status_authorities', 'same_owner_lane',
         'proof_admissions', 'quorum_rounds')


def once(text, old, new):
    assert text.count(old) == 1, (old[:100], text.count(old))
    return text.replace(old, new, 1)


def apply(root):
    root = root.resolve()
    assert not (root / '.git').exists(), 'export first; never instrument a checkout'
    marker = root / 'an-probe-manifest.json'
    assert not marker.exists(), 'fresh exported tree required'
    raft = root / 'src/server/raft_host.c'
    upload = root / 'src/server/put_upload.c'
    before = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in (raft, upload)}
    a, b = raft.read_text(), upload.read_text()
    header = ('#include <stdint.h>\n#include <stdio.h>\n#include <time.h>\n'
              'enum { ' + ', '.join('AN_' + n.upper() for n in NAMES) + ', AN_COUNT };\n'
              'void efs_an_probe_count(unsigned);\n'
              'void efs_an_probe_dump(void);\n'
              'void efs_an_probe_timing(uint64_t, uint64_t);\n'
              'extern __thread unsigned efs_an_owner_context;\n')
    a = '#include "an_probe.h"\n' + a
    b = '#include "an_probe.h"\n' + b
    b = once(b, '    int rc=put_upload(s,body,bytes,path_used);',
             '    efs_an_probe_count(AN_FRAGMENT_ATTEMPTS);\n'
             '    int rc=put_upload(s,body,bytes,path_used);\n'
             '    efs_an_probe_count(rc==EFS_OK?AN_FRAGMENT_OK:AN_FRAGMENT_ERROR);')
    b = once(b, '    server_raft_host_submit(query,sizeof(query),&authority);',
             '    efs_an_probe_count(AN_STATUS_QUERIES);\n'
             '    server_raft_host_submit(query,sizeof(query),&authority);')
    rpc_start=a.index('static int host_rpc_submit(')
    rpc_end=a.index('/* Submit cmd',rpc_start)
    rpc=once(a[rpc_start:rpc_end], '    if (efs_conn_recv_msg(pc, &rtype, &reply, &rlen) != 0 ||',
             '    if(clen && cmd[0]==EFS_MD_REQ_PUT_TICKET_STATUS)\n'
             '        efs_an_probe_count(AN_STATUS_WIRE);\n'
             '    if((clen>=2 && cmd[0]==EFS_MD_CMD_SESSION && cmd[1]==HOST_SESS_GET) ||\n'
             '       (!clen && efs_an_owner_context))efs_an_probe_count(AN_OWNER_WIRE);\n'
             '    if (efs_conn_recv_msg(pc, &rtype, &reply, &rlen) != 0 ||')
    a=a[:rpc_start]+rpc+a[rpc_end:]
    start = a.index('    if(clen && (cmd[0]==EFS_MD_CMD_PUT_TICKET_ADMIT ||')
    end = a.index('    if(clen>=2 && cmd[0]==EFS_MD_CMD_SESSION &&', start)
    block = a[start:end]
    call = '        rc=host_session_establish_admit(h,admission,sizeof(admission),group,&hint);'
    block = once(block, call,
                 '        unsigned an_previous=efs_an_owner_context;\n'
                 '        if(cmd[0]==EFS_MD_REQ_PUT_TICKET_STATUS){\n'
                 '            efs_an_probe_count(AN_STATUS_AUTHORITIES);\n'
                 '            if(efs_raft_shard_group(efs_kv_session_shard(ticket->id.client_uuid))==group)\n'
                 '                efs_an_probe_count(AN_SAME_OWNER_LANE);\n'
                 '            efs_an_owner_context=1;\n'
                 '        }\n' + call + '\n'
                 '        efs_an_owner_context=an_previous;')
    a = a[:start] + block + a[end:]
    a = once(a, '    *a=(struct host_read_admission){v->commit,begun+1,v->term};',
             '    *a=(struct host_read_admission){v->commit,begun+1,v->term};\n'
             '    efs_an_probe_count(AN_PROOF_ADMISSIONS);')
    a = once(a, '                                     efs_raft_term(r)};',
             '                                     efs_raft_term(r)};\n'
             '    efs_an_probe_count(AN_PROOF_ADMISSIONS);')
    a = once(a, '    *started = rc == EFS_OK;\n    return rc;',
             '    *started = rc == EFS_OK;\n'
             '    if(rc==EFS_OK && efs_raft_read_round(r)>previous)\n'
             '        efs_an_probe_count(AN_QUORUM_ROUNDS);\n    return rc;')
    a = once(a, '    return efs_raft_read_begin(r);',
             '    uint64_t an_previous=efs_raft_read_round(r);\n'
             '    int an_rc=efs_raft_read_begin(r);\n'
             '    if(an_rc==EFS_OK && efs_raft_read_round(r)>an_previous)\n'
             '        efs_an_probe_count(AN_QUORUM_ROUNDS);\n    return an_rc;')
    a = once(a, '    if (!force && now_ms - h->obs_last_dump_ms < 5000)\n        return;',
             '    if (!force && now_ms - h->obs_last_dump_ms < 5000)\n        return;\n'
             '    efs_an_probe_dump();')
    a = once(a, '        uint64_t phase_us = batch_obs ? efs_batch_obs_now_us() : 0;',
             '        uint64_t phase_us = batch_obs ? efs_batch_obs_now_us() : 0;\n'
             '        uint64_t an_cycle_start = obs ? efs_batch_obs_now_us() : 0;')
    a = once(a, '        if (batch_obs) BO_HIST(BH_PUMP_LOCK_US, efs_batch_obs_now_us() - phase_us);',
             '        uint64_t an_hold_start = obs ? efs_batch_obs_now_us() : 0;\n'
             '        if (batch_obs) BO_HIST(BH_PUMP_LOCK_US, efs_batch_obs_now_us() - phase_us);')
    a = once(a, '        pthread_mutex_unlock(&h->mu);\n        /* iostats:',
             '        uint64_t an_hold_us = obs ? efs_batch_obs_now_us() - an_hold_start : 0;\n'
             '        pthread_mutex_unlock(&h->mu);\n        /* iostats:')
    a = once(a, '        /* Sleep until the next raft timer tick OR an event (inbox message,',
             '        if (obs) efs_an_probe_timing(efs_batch_obs_now_us() - an_cycle_start, an_hold_us);\n'
             '        /* Sleep until the next raft timer tick OR an event (inbox message,')
    fmt = 'an-proof: t_us=%llu ' + ' '.join(n + '=%llu' for n in NAMES) + '\\n'
    args = ',\n        '.join('(unsigned long long)__atomic_load_n(&an_counts[%d],__ATOMIC_RELAXED)' % i
                            for i in range(len(NAMES)))
    a += ('\n/* Temporary exported-tree AN observation only. */\n'
          '__thread unsigned efs_an_owner_context;\n'
          'static uint64_t an_counts[AN_COUNT];\n'
          '/* Only the pump thread records and dumps these interval maxima. */\n'
          'static uint64_t an_cycle_max_us, an_hold_max_us;\n'
          'void efs_an_probe_timing(uint64_t cycle, uint64_t hold){\n'
          '    if(cycle>an_cycle_max_us)an_cycle_max_us=cycle;\n'
          '    if(hold>an_hold_max_us)an_hold_max_us=hold;\n}\n'
          'void efs_an_probe_count(unsigned i){\n'
          '    if(i<AN_COUNT)__atomic_fetch_add(&an_counts[i],1,__ATOMIC_RELAXED);\n}\n'
          'void efs_an_probe_dump(void){\n'
          '    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);\n'
          '    fprintf(stderr,"' + fmt + '",\n'
          '        (unsigned long long)t.tv_sec*1000000ull+t.tv_nsec/1000,\n        ' + args + ');\n'
          '    fprintf(stderr,"an-pump: active_cycle_max_us=%llu mutex_hold_max_us=%llu\\n",\n'
          '        (unsigned long long)an_cycle_max_us,(unsigned long long)an_hold_max_us);\n'
          '    an_cycle_max_us=an_hold_max_us=0;\n}\n')
    (root / 'src/server/an_probe.h').write_text(header)
    raft.write_text(a)
    upload.write_text(b)
    after = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in (raft, upload, root / 'src/server/an_probe.h')}
    marker.write_text(json.dumps(dict(before=before, after=after, counters=NAMES,
                                    scope='temporary measurement; no behavior changes'), indent=2) + '\n')
    print(marker)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exported_tree', type=Path)
    apply(parser.parse_args().exported_tree)
