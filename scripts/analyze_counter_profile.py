#!/usr/bin/env python3
"""Summarize Task AD perf captures while their matching daemon ELF exists.

Memory levels describe sampled accesses to the counter storage's cache lines,
not coherence ownership or proof of false sharing. Sampling is not a counter
of every access. Period-weighted cycles include kernel CPU in the denominator.
"""
import argparse
from collections import Counter, defaultdict
from datetime import datetime
import json
from pathlib import Path
import re
import subprocess


def output(*args):
    return subprocess.check_output(args, text=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('case', type=Path)
    p.add_argument('elf', type=Path)
    a = p.parse_args()
    metadata = json.loads((a.case/'measurement.json').read_text())
    build_id = re.search(r'Build ID:\s+(\w+)', output('readelf', '-n', str(a.elf)))[1]
    recorded_ids = output('perf', 'buildid-list', '-i', str(a.case/'cycles.data'))
    assert any(row.split()[0] == build_id for row in recorded_ids.splitlines()), \
        'provided ELF does not match this perf capture'
    groups = {'iostats_add': ('efs_iostats_add',),
              'clock_helper': ('efs_iostats_now_us',),
              'writer_diagnostics': ('stats_time', 'stats_max')}
    pie = bool(re.search(r'Type:\s+DYN', output('readelf', '-h', str(a.elf))))
    elf_symbols = [row.split() for row in output('nm', '-S', str(a.elf)).splitlines()
                   if len(row.split()) == 4]
    bases = {}
    recorded_image = str(Path(metadata['arguments']['source'])/'tests/faults/efsd')
    for node, pid in metadata['pids'].items():
        maps = (a.case/f'n{node}.maps').read_text().splitlines()
        assert any(row.endswith(recorded_image) for row in maps), 'daemon mapping absent'
        bases[int(pid)] = next(int(row.split('-')[0], 16) for row in maps
                              if row.split()[2] == '00000000' and row.endswith(recorded_image)) if pie else 0
    code = {name: [(int(row[0], 16), int(row[1], 16)) for row in elf_symbols
                   if any(row[3] == s or row[3].startswith(s+'.') for s in names)]
            for name, names in groups.items()}
    periods, samples = defaultdict(Counter), defaultdict(Counter)
    # This perf version only resolves symbols/addresses when IP is requested.
    command = ['perf', 'script', '-G', '-i', str(a.case/'cycles.data'), '-F', 'pid,period,ip,sym']
    with subprocess.Popen(command, stdout=subprocess.PIPE, text=True) as proc:
        for line in proc.stdout:
            fields = line.split()
            if len(fields) < 3 or not fields[0].isdigit() or not fields[1].isdigit():
                raise ValueError(f'unparsed cycle sample: {line!r}')
            pid, period, ip = int(fields[0]), int(fields[1]), int(fields[2], 16)
            symbol = fields[3] if len(fields) >= 4 else '[unresolved]'
            periods[pid]['all'] += period
            samples[pid]['all'] += 1
            if ip == 0:
                periods[pid]['unlocated_ip'] += period
                samples[pid]['unlocated_ip'] += 1
            if symbol == '[unresolved]' or symbol == '[unknown]':
                periods[pid]['unresolved'] += period
                samples[pid]['unresolved'] += 1
            for name in groups:
                # Known ELF code ranges remain attributable even if perf cannot
                # print a symbol. Unknown kernel/library IPs cannot hide here.
                if any(bases[pid]+lo <= ip < bases[pid]+lo+size for lo, size in code[name]):
                    periods[pid][name] += period
                    samples[pid][name] += 1
        assert proc.wait() == 0
    wanted = {int(pid) for pid in metadata['pids'].values()}
    assert set(periods) <= wanted and periods, (set(periods), wanted)
    cycle_summary = {}
    for pid in sorted(wanted):
        cycle_summary[str(pid)] = {
            'periods': dict(periods[pid]), 'samples': dict(samples[pid]),
            'counter_plus_unlocated_ip_percent': 100*(periods[pid]['iostats_add']+
                periods[pid]['unlocated_ip'])/periods[pid]['all'] if periods[pid]['all'] else None,
            'percent': {name: 100*periods[pid][name]/periods[pid]['all']
                        for name in groups} if periods[pid]['all'] else {}}
    totals = sum((periods[pid] for pid in wanted), Counter())
    cycle_summary['aggregate'] = {'periods': dict(totals),
        'samples': dict(sum((samples[pid] for pid in wanted), Counter())),
        'counter_plus_unlocated_ip_percent': 100*(totals['iostats_add']+totals['unlocated_ip'])/totals['all'],
        'percent': {name: 100*totals[name]/totals['all'] for name in groups}}

    # ELF symbols are absolute for EXEC, relative to the image base for DYN.
    symbols = {}
    for fields in elf_symbols:
        if len(fields) == 4 and fields[3] in ('g_cls', 'g_stats'):
            symbols[fields[3]] = (int(fields[0], 16), int(fields[1], 16))
    assert 'g_cls' in symbols
    ranges = {}
    for node, pid in metadata['pids'].items():
        base = bases[int(pid)]
        ranges[int(pid)] = {name: ((base+addr)//64*64, (base+addr+size+63)//64*64)
                            for name, (addr, size) in symbols.items()}
    memory = defaultdict(Counter)
    command = ['perf', 'script', '-i', str(a.case/'memory.data'), '-F', 'pid,ip,addr,data_src']
    with subprocess.Popen(command, stdout=subprocess.PIPE, text=True) as proc:
        for line in proc.stdout:
            # perf prints addr/data_src before IP regardless of the field order.
            m = re.match(r'\s*(\d+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(.*)', line)
            if not m:
                raise ValueError(f'unparsed IBS sample: {line!r}')
            pid, address = int(m[1]), int(m[2], 16)
            memory['all']['samples'] += 1
            if address:
                memory['all']['nonzero_address_samples'] += 1
            for name, (lo, hi) in ranges.get(pid, {}).items():
                if lo <= address < hi:
                    level = re.search(r'\|LVL ([^|]+)', m[4])
                    memory[name]['samples'] += 1
                    memory[name][level[1].strip() if level else 'unclassified'] += 1
        assert proc.wait() == 0
    d4 = json.loads((a.case/'d4/d4-result.json').read_text())
    working = d4['measured'].get('working_set')
    operations = (sum(working[key] for key in ('overwritten', 'truncated', 'rewritten'))
                  if working else d4['measured']['load_created'])
    lat = json.loads((a.case/'stat-syscalls.json').read_text())
    seq = sorted(lat['seconds'])
    observations = {}
    for node in metadata['pids']:
        peaks = Counter()
        for line in (a.case/f'n{node}.log').read_text(errors='replace').splitlines():
            if ' raft-obs: ' not in line:
                continue
            stamp = datetime.fromisoformat(line.split()[0].replace('Z', '+00:00')).timestamp()
            if metadata['profile_start_epoch_s'] <= stamp <= metadata['profile_end_epoch_s']:
                for name, value in re.findall(r'(pump_hold_max|lock_wait_max|drain_max|tick_max|apply_max|persist_max)=(\d+)us', line):
                    peaks[name] = max(peaks[name], int(value))
        observations[node] = dict(peaks)
    result = {'cycles': cycle_summary, 'memory_cachelines': dict(memory),
              'elf_build_id': build_id,
              'counter_symbols': symbols, 'elf_pie': pie,
              'function_ranges': code, 'raft_obs_window_max_us': observations,
              'daemon_cpu_s': metadata.get('profile_cpu_s'),
              'd4_verdict': d4['verdict'], 'd4_reason': d4['reason'],
              'application_operations_per_configured_load_second': operations/d4['fixture']['d4_load_s'],
              'application_operation_kind': 'rewrite mutations' if working else 'creates',
              'stat_syscall_n': len(seq), 'stat_syscall_errors': lat['errors'],
              'stat_syscall_p50_ms': 1000*seq[len(seq)//2] if seq else None,
              'stat_syscall_p99_ms': 1000*seq[min(len(seq)-1, int(len(seq)*.99))] if seq else None,
              'sampling_limit': 'No statistical independence assumed; no unsampled-access or coherence claim.'}
    (a.case/'profile-summary.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
