#!/usr/bin/env python3
"""Strict W23 TSV serialization/reduction. Missing observations remain NA."""
import argparse
import csv
import sys

HEADER = ['ts', 'node', 'mt_bytes', 'l0_bytes', 'n_l0', 'n_l1', 'rss_kb',
          'lag_max', 'pump_hold_max_us', 'apply_sleep_n', 'backpressure_n', 'role']
METRICS = {'peak_mt_bytes': 'mt_bytes', 'peak_l0_bytes': 'l0_bytes',
           'peak_n_l0': 'n_l0', 'peak_rss_kb': 'rss_kb', 'peak_lag': 'lag_max',
           'max_pump_hold_us': 'pump_hold_max_us', 'apply_sleep_lines': 'apply_sleep_n',
           'backpressure_lines': 'backpressure_n'}

def validate(row):
    if len(row) != len(HEADER):
        raise ValueError(f'expected 12 fields, got {len(row)}')
    if row[1] not in ('1', '2', '3'):
        raise ValueError('invalid node')
    for name, value in zip(HEADER[2:11], row[2:11]):
        if value != 'NA' and (not value.isascii() or not value.isdigit()):
            raise ValueError(f'{name}: invalid numeric observation {value!r}')
    if any('\t' in value or '\n' in value or '\r' in value for value in row):
        raise ValueError('embedded record separator')
    return row

def row(ts, node, kv, rss, lag, pump, apply, pressure, role):
    fields = kv.split() if kv else ['NA'] * 4
    if len(fields) != 4:
        raise ValueError('KV observation must contain four fields')
    return validate([ts, node, *fields,
                     *[value or 'NA' for value in (rss, lag, pump, apply, pressure)], role or '?'])

def peaks(stream):
    reader = csv.reader(stream, delimiter='\t')
    if next(reader, None) != HEADER:
        raise ValueError('unexpected TSV header')
    data = {str(node): {} for node in range(1, 4)}
    for line, values in enumerate(reader, 2):
        try:
            validate(values)
        except ValueError as error:
            raise ValueError(f'line {line}: {error}') from error
        node = data[values[1]]
        for name, value in zip(HEADER[2:11], values[2:11]):
            if value != 'NA':
                node[name] = max(node.get(name, 0), int(value))
    return [f'node{node} ' + ' '.join(f'{label}={values.get(key, "NA")}'
                                    for label, key in METRICS.items())
            for node, values in data.items()]

def cap_proof(stream):
    """Require complete simultaneous observations and a real follower cap.

    This proves pressure was observed, never a memory or recovery bound.
    Raw logs, queue accounting and acknowledged-byte oracles remain separate.
    """
    reader = csv.reader(stream, delimiter='\t')
    if next(reader, None) != HEADER:
        raise ValueError('unexpected TSV header')
    ticks = {}
    cap = False
    required = ('mt_bytes', 'l0_bytes', 'n_l0', 'rss_kb', 'lag_max')
    for line, values in enumerate(reader, 2):
        validate(values)
        record = dict(zip(HEADER, values))
        for field in required:
            if record[field] == 'NA':
                raise ValueError(f'line {line}: {field} unavailable for cap proof')
        if int(record['rss_kb']) == 0:
            raise ValueError(f'line {line}: zero RSS is not a live replica sample')
        nodes = ticks.setdefault(record['ts'], set())
        if record['node'] in nodes:
            raise ValueError('duplicate node/timestamp')
        nodes.add(record['node'])
        roles = set(record['role'].rstrip(',').split(','))
        if not roles or not roles <= {'FOLLOWER', 'LEADER', 'CANDIDATE'}:
            raise ValueError('unavailable or unknown replica role')
        if roles == {'FOLLOWER'} and int(record['l0_bytes']) >= (1 << 30) and int(record['n_l0']) > 0:
            cap = True
    if len(ticks) < 2 or any(nodes != {'1', '2', '3'} for nodes in ticks.values()):
        raise ValueError('at least two complete three-replica sample ticks required')
    if not cap:
        raise ValueError('no follower observed at the unchanged production 1 GiB L0 cap')
    return 'production-cap follower observation confirmed; memory/lag/recovery bounds remain separate gates'

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['row', 'peaks', 'cap-proof'])
    parser.add_argument('values', nargs='*')
    args = parser.parse_args()
    try:
        if args.command == 'row':
            if len(args.values) != 9:
                raise ValueError('row requires timestamp, node, KV observation and six values')
            print('\t'.join(row(*args.values)))
        else:
            if len(args.values) != 1:
                raise ValueError('reduction requires one TSV file')
            with open(args.values[0]) as stream:
                print(cap_proof(stream) if args.command == 'cap-proof' else '\n'.join(peaks(stream)))
    except ValueError as error:
        print(f'W23 samples: {error}', file=sys.stderr)
        sys.exit(1)
