#!/usr/bin/env python3
"""Strict W23 TSV serialization/reduction. Missing observations remain NA."""
import argparse
import csv
import itertools
import json
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

def raw_proof(tsv, raw):
    """Compare every emitted field with its original JSON observation.

    Structural validity alone cannot catch twelve numeric fields reordered by
    a sampler. Preserve unknown counters as NA; do not infer them from a peak.
    """
    reader = csv.reader(tsv, delimiter='\t')
    if next(reader, None) != HEADER:
        raise ValueError('unexpected TSV header')

    def expected_rows():
        for line, text in enumerate(raw, 1):
            try:
                r = json.loads(text)
                kv, pressure = r['kv'], r['pressure']
                if kv is None or pressure is None:
                    continue  # These observations are not emitted by the driver.
                values = [r['ts'], r['node'], kv['mt_bytes'], kv['l0_bytes'],
                          kv['l0'], kv['l1'], r['rss_kb'], r['replication_lag'],
                          (r['timing'] or {}).get('pump_hold_max', 'NA'),
                          'NA', pressure['recv_rejected'],
                          ','.join(g['role'] for g in r['groups'])]
                yield validate([str(value) for value in values])
            except (KeyError, TypeError, json.JSONDecodeError, ValueError) as error:
                raise ValueError(f'raw line {line}: {error}') from error

    count = 0
    for line, (actual, expected) in enumerate(
            itertools.zip_longest(reader, expected_rows()), 2):
        if actual is None or expected is None:
            raise ValueError(f'line {line}: raw/TSV observation count differs')
        validate(actual)
        for field, value, original in zip(HEADER, actual, expected):
            if value != original:
                raise ValueError(f'line {line}: {field}={value!r}, raw={original!r}')
        count += 1
    if count == 0:
        raise ValueError('no complete raw observations')
    return f'{count} TSV observations match every raw JSON field; unavailable counters remain NA'

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['row', 'peaks', 'cap-proof', 'raw-proof'])
    parser.add_argument('values', nargs='*')
    args = parser.parse_args()
    try:
        if args.command == 'row':
            if len(args.values) != 9:
                raise ValueError('row requires timestamp, node, KV observation and six values')
            print('\t'.join(row(*args.values)))
        elif args.command == 'raw-proof':
            if len(args.values) != 2:
                raise ValueError('raw-proof requires TSV and JSONL files')
            with open(args.values[0]) as tsv, open(args.values[1]) as raw:
                print(raw_proof(tsv, raw))
        else:
            if len(args.values) != 1:
                raise ValueError('reduction requires one TSV file')
            with open(args.values[0]) as stream:
                print(cap_proof(stream) if args.command == 'cap-proof' else '\n'.join(peaks(stream)))
    except ValueError as error:
        print(f'W23 samples: {error}', file=sys.stderr)
        sys.exit(1)
