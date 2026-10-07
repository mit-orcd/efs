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

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['row', 'peaks'])
    parser.add_argument('values', nargs='*')
    args = parser.parse_args()
    try:
        if args.command == 'row':
            if len(args.values) != 9:
                raise ValueError('row requires timestamp, node, KV observation and six values')
            print('\t'.join(row(*args.values)))
        else:
            if len(args.values) != 1:
                raise ValueError('peaks requires one TSV file')
            with open(args.values[0]) as stream:
                print('\n'.join(peaks(stream)))
    except ValueError as error:
        print(f'W23 samples: {error}', file=sys.stderr)
        sys.exit(1)
