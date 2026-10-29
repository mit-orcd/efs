#!/usr/bin/env python3
"""Task AI: generate a disposable diagnostic source; summarize bounded records.

This does not alter product locking or qualify RDMA on a machine without verbs.
Use a separate export/build for instrumented runs; keep formal throughput runs
uninstrumented. No environment reader or runtime product mode is introduced.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

LOCK = 'pthread_mutex_lock(&g_reg_lock);'
UNLOCK = 'pthread_mutex_unlock(&g_reg_lock);'
ANCHOR = 'static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;'
FUNCTION = re.compile(r'^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;{}]*?\)\s*\{', re.M)
EXPECTED = {'harvest_recv_wcs', 'conn_create', 'efs_rdma_conn_destroy', 'efs_rdma_totals'}


def instrument(source, header):
    if source.count(ANCHOR) != 1 or 'AI_REGISTRY_META' in source:
        raise ValueError('source anchor missing or already instrumented')
    definitions = [(m.start(), m.group(1)) for m in FUNCTION.finditer(source)]
    names = []
    def lock(match):
        name = next(name for pos, name in reversed(definitions) if pos < match.start())
        if name in names:
            raise ValueError('unexpected multiple registry locks in function')
        names.append(name)
        return f'ai_registry_lock({len(names)-1});'
    edited = re.sub(re.escape(LOCK), lock, source)
    if set(names) != EXPECTED or source.count(UNLOCK) != len(names):
        raise ValueError(f'registry site drift; inspect source before measuring: {names}')
    edited = edited.replace(UNLOCK, 'ai_registry_unlock();')
    prefix = f'\n#define AI_REG_SITES {len(names)}\nstatic const char *ai_site_names[] = {{'+','.join(json.dumps(n) for n in names)+'};\n'
    return edited.replace(ANCHOR, ANCHOR+prefix+header, 1), names


def summarize(lines):
    metadata, rows = [], []
    for line in lines:
        # Timestamped stderr is allowed; reject malformed marked records.
        if 'AI_REGISTRY_META ' in line:
            metadata.append(json.loads(line.split('AI_REGISTRY_META ', 1)[1]))
        elif 'AI_REGISTRY ' in line:
            row = json.loads(line.split('AI_REGISTRY ', 1)[1])
            for field in ('calls','wait_ns','hold_ns','first_ns','last_ns','incomplete'):
                if not isinstance(row[field], int) or row[field] < 0:
                    raise ValueError('invalid counter')
            span = row['last_ns'] - row['first_ns']
            if not row['incomplete'] and (not row['calls'] or span <= 0 or
                                         row['wait_ns']+row['hold_ns'] > span):
                raise ValueError('inconsistent closed interval')
            row.update(span_ns=span,
                       acquisition_interval_share_pct=100*row['wait_ns']/span if span>0 else None,
                       acquisitions_per_span_s=row['calls']*1e9/span if span>0 else None,
                       mean_wait_ns=row['wait_ns']/row['calls'] if row['calls'] else None,
                       mean_hold_ns=row['hold_ns']/row['calls'] if row['calls'] else None)
            rows.append(row)
    keys = [(r['pid'], r['tid'], r['function']) for r in rows]
    if len(keys) != len(set(keys)) or len(metadata) != len({m['pid'] for m in metadata}):
        raise ValueError('duplicate process/thread evidence; summarize each run separately')
    incomplete = (not metadata or not rows or any(m['overflow'] for m in metadata) or
                  any(r['incomplete'] for r in rows) or
                  {r['pid'] for r in rows} != {m['pid'] for m in metadata})
    return dict(verdict='INCOMPLETE' if incomplete else 'DIAGNOSTIC_ONLY',
                metadata=metadata, threads=rows,
                limits='Instrumented acquisition intervals include clock/lock-call overhead; '
                       'span is first acquisition to last release, not the application window. '
                       'No production wait-share, real-RDMA, throughput or correctness PASS inferred.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    i = sub.add_parser('instrument'); i.add_argument('source', type=Path); i.add_argument('output', type=Path)
    a = sub.add_parser('summarize'); a.add_argument('logs', type=Path, nargs='+')
    args = p.parse_args()
    if args.command == 'instrument':
        if args.output.resolve() == args.source.resolve() or args.output.exists():
            raise ValueError('fresh diagnostic output required; never edit shipping source')
        source = args.source.read_text()
        edited, names = instrument(source, Path(__file__).with_suffix('.h').read_text())
        with args.output.open('x') as f: f.write(edited)
        print(json.dumps(dict(source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                              diagnostic_sha256=hashlib.sha256(edited.encode()).hexdigest(), sites=names)))
    else:
        def records():
            for log in args.logs:
                with log.open(errors='replace') as f: yield from f
        print(json.dumps(summarize(records()), indent=2))


if __name__ == '__main__': main()
