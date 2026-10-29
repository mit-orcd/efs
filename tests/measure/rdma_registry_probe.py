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

# Separate follow-up diagnostic: poll() return to the first successful pending
# ring publication. Excludes kernel wake-to-run and NIC-to-CQE delay. This is
# not enabled in formal runs or in the initial registry-only diagnostic build.
WAKE_HEADER = r'''
static __thread uint64_t ai_channel_return_ns;
static uint64_t ai_wake_count, ai_wake_sum_ns, ai_wake_max_ns, ai_wake_hist[32];
static void ai_wake_dispatch(void) {
    if (!ai_channel_return_ns) return;
    uint64_t elapsed = ai_now()-ai_channel_return_ns;
    ai_channel_return_ns=0;
    unsigned bin=0; uint64_t v=elapsed;
    while (v>1 && bin<31) { v>>=1; bin++; }
    __atomic_add_fetch(&ai_wake_count,1,__ATOMIC_RELAXED);
    __atomic_add_fetch(&ai_wake_sum_ns,elapsed,__ATOMIC_RELAXED);
    __atomic_add_fetch(&ai_wake_hist[bin],1,__ATOMIC_RELAXED);
    uint64_t old=__atomic_load_n(&ai_wake_max_ns,__ATOMIC_RELAXED);
    while (elapsed>old && !__atomic_compare_exchange_n(&ai_wake_max_ns,&old,elapsed,0,
            __ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
}
static void __attribute__((destructor)) ai_wake_dump(void) {
    fprintf(stderr,"AI_WAKE {\"pid\":%ld,\"count\":%llu,\"sum_ns\":%llu,\"max_ns\":%llu,\"log2_floor_ns_hist\":[",
        (long)getpid(),(unsigned long long)__atomic_load_n(&ai_wake_count,__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&ai_wake_sum_ns,__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&ai_wake_max_ns,__ATOMIC_RELAXED));
    for(unsigned i=0;i<32;i++) fprintf(stderr,"%s%llu",i?",":"",
        (unsigned long long)__atomic_load_n(&ai_wake_hist[i],__ATOMIC_RELAXED));
    fprintf(stderr,"]}\n");
}
'''
WAKE_ANCHORS = (
    ('static void harvest_recv_wcs(struct ibv_wc *wcs, int n)',
     WAKE_HEADER+'\nstatic void harvest_recv_wcs(struct ibv_wc *wcs, int n)'),
    ('pend_push(rc, buf, wcs[i].byte_len);',
     'pend_push(rc, buf, wcs[i].byte_len);\n                if (!rc->broken) ai_wake_dispatch();'),
    ('int pr = poll(&p, 1, 1000);',
     'int pr = poll(&p, 1, 1000);\n            if (pr > 0 && (p.revents & POLLIN)) ai_channel_return_ns = ai_now();'),
)


def instrument_wake(source):
    if 'AI_REGISTRY_META ' not in source or 'AI_WAKE ' in source:
        raise ValueError('fresh registry diagnostic required')
    for old,new in WAKE_ANCHORS:
        if source.count(old) != 1:
            raise ValueError('wake source drift: '+old)
        source = source.replace(old,new,1)
    return source


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
    w = sub.add_parser('wake'); w.add_argument('source', type=Path); w.add_argument('output', type=Path)
    a = sub.add_parser('summarize'); a.add_argument('logs', type=Path, nargs='+')
    args = p.parse_args()
    if args.command in ('instrument','wake'):
        if args.output.resolve() == args.source.resolve() or args.output.exists():
            raise ValueError('fresh diagnostic output required; never edit shipping source')
        source = args.source.read_text()
        if args.command == 'wake':
            edited, names = instrument_wake(source), []
        else:
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
