#!/usr/bin/env python3
"""AB/AE measurement-only pump timers for archived (non-Git) source copies.

instrument ROOT adds the SAME two batch histograms to baseline and candidate:
actual h->mu hold through logging/unlock, and active pump cycle through its
persistence/logging tail, excluding poll sleep. These are experiment probes,
not another product setting. Keep each resulting source hash with the run.
summary EVIDENCE emits measured metrics and every failure from profile runs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def instrument(root):
    assert not (root / '.git').exists(), 'instrument archived experiment copies only'
    p = root / 'src/server/raft_host.c'
    s = p.read_text()
    a, b = s.index('static void *host_pump('), s.index('static uint64_t make_salt(')
    body = s[a:b]
    assert 'measure_cycle_start' not in body
    marker = '        uint64_t phase_us = batch_obs ? efs_batch_obs_now_us() : 0;'
    assert body.count(marker) == 1
    body = body.replace(marker, marker+'\n        uint64_t measure_cycle_start = phase_us, measure_hold_start = 0;')
    marker = '        pthread_mutex_lock(&h->mu);'
    body = body.replace(marker, marker+'\n        if (batch_obs) measure_hold_start = efs_batch_obs_now_us();', 1)
    # Pick the cycle unlock immediately after the normal diagnostics block,
    # not one of the deliberately released persistence-cohort scopes.
    markers = [m for m in ('        pthread_mutex_unlock(&h->mu);\n        /* iostats:',
                           '        pthread_mutex_unlock(&h->mu);\n        host_obs_roles_report(',
                           '        pthread_mutex_unlock(&h->mu);\n        if (report_due)')
               if body.count(m) == 1]
    assert len(markers) == 1, markers
    marker = markers[0]
    assert body.count(marker) == 1
    body = body.replace(marker, '        if (batch_obs) BO_HIST(BH_MEASURE_PUMP_HOLD_US, efs_batch_obs_now_us()-measure_hold_start);\n'+marker)
    marker = '        /* Sleep until the next raft timer tick'
    assert body.count(marker) == 1
    body = body.replace(marker, '        if (batch_obs) BO_HIST(BH_MEASURE_PUMP_CYCLE_US, efs_batch_obs_now_us()-measure_cycle_start);\n'+marker)
    p.write_text(s[:a]+body+s[b:])
    p = root / 'include/efs/batch_obs.h'
    s = p.read_text().replace('BH_READ_REPLY_INBOX_US, BH_HIST_N',
                             'BH_READ_REPLY_INBOX_US, BH_MEASURE_PUMP_HOLD_US, BH_MEASURE_PUMP_CYCLE_US, BH_HIST_N')
    p.write_text(s)
    p = root / 'src/common/batch_obs.c'
    s = p.read_text().replace('"read_req_inbox_us", "read_reply_inbox_us"',
                             '"read_req_inbox_us", "read_reply_inbox_us", "measure_pump_hold_us", "measure_pump_cycle_us"')
    p.write_text(s)
    print(json.dumps({str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in (root/'src/server/raft_host.c', root/'include/efs/batch_obs.h',
                                root/'src/common/batch_obs.c')}, indent=2))


def summary(root):
    rows = []
    for result in sorted(root.glob('*/result.json')):
        d = json.loads(result.read_text())
        metrics = {}
        for log in result.parent.glob('n[123].log'):
            text = log.read_text(errors='replace')
            h = {}
            for line in text.splitlines():
                if 'batch-obs {' not in line:
                    continue
                try:
                    hist = json.loads(line[line.index('batch-obs ')+10:])['hist']
                except (ValueError, KeyError):
                    continue
                for key in ('pump_lock_us', 'pump_drain_us', 'measure_pump_hold_us', 'measure_pump_cycle_us'):
                    if key in hist:
                        h[key] = hist[key]
            h['raft_hold_max_us'] = max(map(int, re.findall(r'pump_hold_max=(\d+)us', text)), default=None)
            h['overflow'] = max(map(int, re.findall(r'telemetry: overflow=(\d+)', text)), default=0)
            metrics[log.stem] = h
        rows.append(dict(case=result.parent.name, sink=d.get('sink'), obs=d.get('obs'),
                         verdict=d.get('verdict'), error=(d.get('error') or '')[:500], errors=d.get('errors'),
                         write_ops_s=d.get('write_ops_s'), writes=d.get('writes'), late_writes=d.get('late_writes'),
                         stat_p50_s=d.get('stat_p50_s'), stat_p99_s=d.get('stat_p99_s'),
                         daemon_cpu_s=d.get('daemon_cpu_s'), metrics=metrics))
    print(json.dumps(rows, indent=2))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=('instrument', 'summary'))
    p.add_argument('root', type=Path)
    a = p.parse_args()
    (instrument if a.action == 'instrument' else summary)(a.root)
