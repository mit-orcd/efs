#!/usr/bin/env python3
"""Summarize Task AD perf captures while their matching daemon ELF exists.

Memory levels describe sampled accesses to the counter storage's cache lines,
not coherence ownership or proof of false sharing. Sampling is not a counter
of every access. Period-weighted cycles include kernel CPU in the denominator.
"""
import argparse
from collections import Counter, defaultdict
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
    groups = {'iostats_add': ('efs_iostats_add',),
              'clock_helper': ('efs_iostats_now_us',),
              'writer_diagnostics': ('stats_time', 'stats_max')}
    periods, samples = defaultdict(Counter), defaultdict(Counter)
    # This perf version only resolves symbols/addresses when IP is requested.
    command = ['perf', 'script', '-G', '-i', str(a.case/'cycles.data'), '-F', 'pid,period,ip,sym']
    with subprocess.Popen(command, stdout=subprocess.PIPE, text=True) as proc:
        for line in proc.stdout:
            fields = line.split()
            if len(fields) < 3 or not fields[0].isdigit() or not fields[1].isdigit():
                raise ValueError(f'unparsed cycle sample: {line!r}')
            pid, period = int(fields[0]), int(fields[1])
            symbol = fields[3] if len(fields) >= 4 else '[unresolved]'
            periods[pid]['all'] += period
            samples[pid]['all'] += 1
            if symbol == '[unresolved]' or symbol == '[unknown]':
                periods[pid]['unresolved'] += period
                samples[pid]['unresolved'] += 1
            for name, prefixes in groups.items():
                if any(symbol == s or symbol.startswith(s+'.') for s in prefixes):
                    periods[pid][name] += period
                    samples[pid][name] += 1
        assert proc.wait() == 0
    wanted = {int(pid) for pid in metadata['pids'].values()}
    assert set(periods) <= wanted and periods, (set(periods), wanted)
    cycle_summary = {}
    for pid in sorted(wanted):
        cycle_summary[str(pid)] = {
            'periods': dict(periods[pid]), 'samples': dict(samples[pid]),
            'percent': {name: 100*periods[pid][name]/periods[pid]['all']
                        for name in groups} if periods[pid]['all'] else {}}
    totals = sum((periods[pid] for pid in wanted), Counter())
    cycle_summary['aggregate'] = {'periods': dict(totals),
        'samples': dict(sum((samples[pid] for pid in wanted), Counter())),
        'percent': {name: 100*totals[name]/totals['all'] for name in groups}}

    # ELF symbols are absolute for EXEC, relative to the image base for DYN.
    pie = bool(re.search(r'Type:\s+DYN', output('readelf', '-h', str(a.elf))))
    symbols = {}
    for line in output('nm', '-S', str(a.elf)).splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[3] in ('g_cls', 'g_stats'):
            symbols[fields[3]] = (int(fields[0], 16), int(fields[1], 16))
    assert 'g_cls' in symbols
    ranges = {}
    for node, pid in metadata['pids'].items():
        maps = (a.case/f'n{node}.maps').read_text().splitlines()
        base = 0
        if pie:
            base = next(int(row.split('-')[0], 16) for row in maps
                        if row.split()[2] == '00000000' and row.endswith(str(a.elf)))
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
    lat = json.loads((a.case/'stat-syscalls.json').read_text())
    seq = sorted(lat['seconds'])
    result = {'cycles': cycle_summary, 'memory_cachelines': dict(memory),
              'counter_symbols': symbols, 'elf_pie': pie,
              'daemon_cpu_s': metadata.get('profile_cpu_s'),
              'd4_verdict': d4['verdict'], 'd4_reason': d4['reason'],
              'created_per_configured_load_second': d4['measured']['load_created']/
                  d4['fixture']['d4_load_s'],
              'stat_syscall_n': len(seq), 'stat_syscall_errors': lat['errors'],
              'stat_syscall_p50_ms': 1000*seq[len(seq)//2] if seq else None,
              'stat_syscall_p99_ms': 1000*seq[min(len(seq)-1, int(len(seq)*.99))] if seq else None,
              'sampling_limit': 'No statistical independence assumed; no unsampled-access or coherence claim.'}
    (a.case/'profile-summary.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
