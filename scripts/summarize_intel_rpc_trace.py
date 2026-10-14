#!/usr/bin/env python3
"""Summarize bounded strace -f -ttt -T -yy -xx -s 8 RPC headers, offline.

Pair complete sendmsg/writev frames with complete replies on exclusively
checked-out TCP descriptors. Do not treat EAGAIN, redirects or syscall counts
as failed RPCs. Exclude partial/unmatched exchanges; report their counts.
Timings include ptrace/scheduling overhead and exclude connection checkout.
"""
import argparse
import collections
import json
from pathlib import Path
import re


def syscalls(path):
    pending = {}
    for line in path.open(errors='replace'):
        m = re.match(r'\s*(\d+)\s+(\d+\.\d+)\s+(.*)', line)
        if not m:
            continue
        tid, stamp, body = int(m[1]), float(m[2]), m[3]
        if '<unfinished ...>' in body:
            pending[tid] = (stamp, body.split('<unfinished ...>')[0])
            continue
        if body.startswith('<... '):
            old = pending.pop(tid, None)
            if not old:
                continue
            stamp, before = old
            body = before + body.split(' resumed>', 1)[-1]
        result = re.search(r' = (-?\d+)(?: ([A-Z0-9_]+))?.*<([\d.]+)>$', body)
        if result:
            yield stamp, stamp+float(result[3]), body, int(result[1]), result[2]


def quantiles(values):
    values = sorted(values)
    return dict(count=len(values), mean_ms=sum(values)/len(values)*1000,
                p50_ms=values[(len(values)-1)//2]*1000,
                p95_ms=values[int((len(values)-1)*.95)]*1000,
                max_ms=values[-1]*1000, over_1s=sum(v >= 1 for v in values))


def summarize(path, names):
    live = {}
    counts = collections.Counter()
    sent = collections.Counter()
    statuses = collections.defaultdict(collections.Counter)
    times = collections.defaultdict(list)
    exchanges = []
    endpoints = collections.defaultdict(list)
    read_hints = collections.Counter()
    report_shapes = collections.defaultdict(collections.Counter)
    for start, end, body, rc, error in syscalls(path):
        call = re.match(r'(sendmsg|writev|recvfrom)\((\d+)<TCP:\[([^]]+)\]>', body)
        if not call:
            continue
        op, fd = call[1], (int(call[2]), call[3])
        if rc < 0:
            counts[op+'/'+str(error)] += 1
            continue
        if op != 'recvfrom':
            vectors = re.findall(r'iov_base="((?:\\x[0-9a-f]{2})+)"(?:\.\.\.)?, iov_len=(\d+)', body)
            if not vectors:
                continue
            head = bytes.fromhex(vectors[0][0].replace('\\x', ''))
            if len(head) != 5 or int(vectors[0][1]) != 5:
                counts['unrecognized_send'] += 1
                continue
            size = int.from_bytes(head[:4], 'big')+4
            if rc != size:
                counts['partial_send'] += 1
                live.pop(fd, None)
                continue
            if fd in live:
                counts['request_without_captured_reply'] += 1
            kind = head[4]
            sent[kind] += 1
            request_prefix = (bytes.fromhex(vectors[1][0].replace('\\x', ''))
                              if len(vectors) > 1 else b'')
            report_count = (int.from_bytes(request_prefix[4:8], 'little')
                            if kind == 67 and len(request_prefix) >= 8 else None)
            live[fd] = dict(kind=kind, start=end, wire_bytes=size,
                            report_count=report_count, header=bytearray(),
                            length=None, body_count=0, prefix=bytearray())
            continue
        state = live.get(fd)
        if not state or not rc:
            counts['recv_without_captured_send'] += 1
            continue
        buf = re.search(r',\s*"((?:\\x[0-9a-f]{2})*)"', body)
        if not buf:
            counts['unrecognized_recv'] += 1
            live.pop(fd, None)
            continue
        data = bytes.fromhex(buf[1].replace('\\x', ''))
        if state['length'] is None:
            need = 4-len(state['header'])
            if rc > need or len(data) < rc:
                counts['unrecognized_header'] += 1
                live.pop(fd, None)
                continue
            state['header'].extend(data[:rc])
            if len(state['header']) == 4:
                state['length'] = int.from_bytes(state['header'], 'big')
            continue
        if state['body_count'] < 3:
            need = 3-len(state['prefix'])
            state['prefix'].extend(data[:min(need, rc)])
        state['body_count'] += rc
        if state['body_count'] > state['length']:
            counts['overlong_reply'] += 1
            live.pop(fd, None)
        elif state['body_count'] == state['length']:
            live.pop(fd, None)
            prefix = state['prefix']
            if len(prefix) < 2:
                counts['short_reply'] += 1
                continue
            request = state['kind']
            if prefix[0] != request+1:
                counts['reply_type_mismatch'] += 1
                continue
            if request == 67:
                shape = f"chunks={state['report_count']},wire_bytes={state['wire_bytes']}"
                report_shapes[shape][str(prefix[1])] += 1
            label = names.get(request, str(request))
            duration = end-state['start']
            if request in (43,47) and len(prefix) >= 3 and prefix[1] == 0:
                read_hints[label+' node='+str(prefix[2])] += 1
            statuses[label][str(prefix[1])] += 1
            times[label].append(duration)
            endpoints[label+' '+fd[1]].append(duration)
            exchanges.append(dict(request=request, reply=prefix[0], status=prefix[1],
                                  seconds=duration, endpoint=fd[1]))
    return dict(sent={names.get(k, str(k)):v for k,v in sent.items()},
                replies={k:dict(v) for k,v in statuses.items()},
                exchange_timings={k:quantiles(v) for k,v in times.items()},
                by_endpoint={k:quantiles(v) for k,v in endpoints.items()},
                read_serving_hints=dict(read_hints),
                report_shapes={k:dict(v) for k,v in report_shapes.items()},
                diagnostics=dict(counts), pending_at_end=len(live),
                longest=sorted(exchanges, key=lambda x:x['seconds'], reverse=True)[:12])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('trace', type=Path)
    a = ap.parse_args()
    header = Path(__file__).resolve().parents[1]/'include/efs/protocol.h'
    names = {int(v):k for k,v in re.findall(r'\b(EFS_MSG_\w+)\s*=\s*(\d+)', header.read_text())}
    print(json.dumps(summarize(a.trace, names), indent=2))


if __name__ == '__main__':
    main()
