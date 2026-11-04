#!/usr/bin/env python3
"""Insert bounded measurement probes in an isolated AF source export only."""
from pathlib import Path
import argparse
p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);a=p.parse_args()
root=a.source.resolve();assert '/efs-task-af/' in str(root),root
path=root/'src/server/raft_host.c';s=path.read_text();assert 'af-probe' not in s
s=Path(__file__).with_name('af_probe.h').read_text()+'\n'+s
start=s.index('static void *host_pump(void *arg)\n{');end=s.index('static uint64_t make_salt',start)
part=s[start:end].replace('    uint64_t last_tick_us = 0;','    uint64_t af_previous = 0;\n    uint64_t last_tick_us = 0;')
anchor='        struct pollfd pfd;';assert part.count(anchor)==1
part=part.replace(anchor,'''        uint64_t af_cycle=af_now();
        if(af_enabled && af_previous)af_record(1,af_cycle-af_previous);
        af_previous=af_enabled?af_cycle:0;
'''+anchor)
anchor='        memset(&pfd, 0, sizeof(pfd));';assert part.count(anchor)==1
part=part.replace(anchor,'        af_record(0,af_now()-af_cycle);\n'+anchor)
s=s[:start]+part+s[end:]
start=s.index('void server_raft_host_status(');end=s.index('void server_raft_host_resync(',start)
part=s[start:end];lock='    pthread_mutex_lock(&h->mu);';unlock='    pthread_mutex_unlock(&h->mu);'
assert part.count(lock)==part.count(unlock)==1
part=part.replace(lock,'    uint64_t af_begin=af_now();\n'+lock+'\n    uint64_t af_held=af_now();\n    af_record(2,af_held-af_begin);')
part=part.replace(unlock,'    af_record(3,af_now()-af_held);\n'+unlock)
s=s[:start]+part+s[end:];path.write_text(s)
print('AF isolated status/pump instrumentation applied; product sources unchanged')
