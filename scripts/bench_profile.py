#!/usr/bin/env python3
"""Run isolated benchmark cases and retain evidence for CPU hot-path analysis."""
import argparse
import datetime
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCAL_MODES = ['io', 'io-blake3', 'blake3', 'data', 'meta']
DEFAULT_MODES = ','.join(LOCAL_MODES)


def integers(value):
    try:
        values = list(dict.fromkeys(int(v) for v in value.split(',')))
    except ValueError as exc:
        raise argparse.ArgumentTypeError('expected comma-separated integers') from exc
    if not values or any(v < 0 for v in values):
        raise argparse.ArgumentTypeError('values must be nonnegative')
    return values


def writers(value):
    values = []
    for v in value.split(','):
        if v == 'auto':
            values.append(v)
        elif v.isdigit() and 0 <= int(v) <= 64:
            values.append(int(v))
        else:
            raise argparse.ArgumentTypeError('writers must be 0..64 or auto')
    return list(dict.fromkeys(values))


def sizes(value):
    out = []
    for item in value.split(','):
        m = re.fullmatch(r'([1-9][0-9]*)([KkMmGg]?)', item)
        if not m:
            raise argparse.ArgumentTypeError('sizes must be positive bytes or K/M/G')
        n = int(m[1]) * {'': 1, 'k': 1024, 'm': 1024**2, 'g': 1024**3}[m[2].lower()]
        if n > 1024**3:
            raise argparse.ArgumentTypeError('maximum size is 1 GiB')
        out.append(n)
    return list(dict.fromkeys(out))


def parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument('--binary', type=Path, default=ROOT / 'efs-bench')
    p.add_argument('--output', type=Path, help='new result directory; refuses existing paths')
    p.add_argument('--storage-root', type=Path, action='append', default=[],
                   help='existing scratch parent on a storage device; repeat for multiple paths')
    p.add_argument('--require-distinct-devices', action='store_true', help='require at least two storage roots on distinct filesystem devices')
    p.add_argument('--io-write-layouts', choices=['both', 'allocating', 'preallocated'], default='both', help='compare initial allocation with a populated preallocated overwrite window')
    p.add_argument('--time', type=float, default=3, help='seconds per timed benchmark phase')
    p.add_argument('--io-sizes', type=sizes, default=sizes('4K,64K,1M'), help='raw I/O sizes, identical with/without BLAKE3')
    p.add_argument('--io-sync', action='store_true', help='include fdatasync in each raw write operation')
    p.add_argument('--hash-sizes', type=sizes, default=sizes('4K,64K,128K,1M'))
    p.add_argument('--threads', type=integers, help='BLAKE3 thread ladder; default 1,2,4,affinity CPU count')
    p.add_argument('--writers', type=writers, default=writers('0,2,auto'), help='inline, fixed pool and automatic pool variants')
    p.add_argument('--data-sync', action='store_true', help='benchmark-only O_SYNC engine writes')
    p.add_argument('--data-rw', choices=['both', 'split', 'read', 'write'], default='both', help='split records isolated write and populated-read cases')
    p.add_argument('--data-full-paths', action='store_true', help='measure all selected roots together without the engine prefix ladder')
    p.add_argument('--writer-stats', action='store_true', help='opt-in engine writer admission, handoff and service timing')
    p.add_argument('--data-size', type=sizes, default=sizes('256M'), help='bounded local data working set across all QD slots (fragment bytes)')
    p.add_argument('--qds', type=integers, default=integers('1,16,64,256'))
    p.add_argument('--storage-principles', action='store_true',
                   help='select independent EFS-shaped read/write/delete principle experiments; replaces the default modes')
    p.add_argument('--principles-sizes', type=sizes, default=sizes('64K'), help='principle payload sizes; aligned, 4 KiB..1 MiB')
    p.add_argument('--principles-objects', type=int, default=4096, help='total records per finite principle case (not per worker); --time does not apply')
    p.add_argument('--principles-batch', type=int, default=32, help='records per worker synchronization batch')
    p.add_argument('--modes', default=DEFAULT_MODES, help='comma-separated modes; all adds remote modes')
    p.add_argument('--seed', help='seed host:port; adds net,store,read,remote-meta to the default matrix')
    p.add_argument('--export', default='efs-test', help='export for remote metadata benchmark')
    p.add_argument('--files', type=int, default=5000, help='remote metadata files')
    p.add_argument('--dirs', type=int, default=64, help='remote metadata directories')
    p.add_argument('--store-size', type=sizes, default=sizes('256M'), help='bounded remote read/write working set')
    p.add_argument('--chunk-base', type=int, default=0, help='remote benchmark chunk-index base; avoid other writers')
    p.add_argument('--strace', action='store_true', help='separate syscall-traced rerun, after baseline and perf')
    p.add_argument('--strace-expr', help='strace -e expression, e.g. trace=writev,fsync,futex')
    p.add_argument('--frequency', type=int, default=499)
    p.add_argument('--event', default='cycles', help='perf sampling event; use cpu-clock on VMs without PMU')
    p.add_argument('--call-graph', choices=['auto', 'fp', 'dwarf'], default='auto', help='auto uses DWARF for engine data/libc stacks and frame pointers elsewhere; DWARF recordings are larger')
    p.add_argument('--report-jobs', type=int, default=4, help='parallel case report jobs after workloads finish (each runs one perf process at a time)')
    p.add_argument('--reports-only', type=Path, help='regenerate reports/analysis in an existing run; never execute workloads')
    p.add_argument('--no-perf', action='store_true', help='explicit unprofiled run; analysis marks profiles absent')
    p.add_argument('--skip-ceiling', action='store_true', help='omit fio/raw ceiling probe; engine tests still run')
    p.add_argument('--timeout', type=float, default=600, help='timeout per subprocess (including each workload)')
    p.add_argument('--dry-run', action='store_true', help='print matrix; do not build, create scratch or run tools')
    p.add_argument('--analyze', type=Path, help='regenerate analysis from a completed/interrupted result directory')
    return p


def call_graph_for(a, mode):
    if a.call_graph != 'auto':
        return a.call_graph
    return 'dwarf' if mode == 'data' else 'fp'


def bad_callchain_addresses(path, virtual_bits):
    if not path.exists() or not virtual_bits or not 1 < virtual_bits < 64:
        return []
    bad = []
    for token in sorted(set(re.findall(r'0x[0-9a-fA-F]+', path.read_text(errors='replace')))):
        address = int(token, 16)
        upper = address >> virtual_bits
        expected = (1 << (64 - virtual_bits)) - 1 if (address >> (virtual_bits - 1)) & 1 else 0
        if address >= 1 << 64 or upper != expected:
            bad.append(token)
    return bad


def cases_for(a, cpus):
    if len(a.storage_root) > 24:
        raise ValueError('maximum 24 storage roots, matching the engine limit')
    if a.storage_principles and (a.modes != DEFAULT_MODES or a.seed):
        raise ValueError('--storage-principles cannot be combined with --modes or --seed')
    modes = ['principles'] if a.storage_principles else a.modes.split(',')
    remote = ['net', 'store', 'read', 'remote-meta']
    if modes == ['all']:
        modes = LOCAL_MODES + remote
    elif a.seed and a.modes == DEFAULT_MODES:
        modes += remote
    if not modes or len(set(modes)) != len(modes) or any(m not in LOCAL_MODES + remote + ['principles'] for m in modes):
        raise ValueError('unknown --modes entry')
    if any(m in remote for m in modes) and not a.seed:
        raise ValueError('remote modes require --seed')
    threads = a.threads or sorted(set([1, min(2, cpus), min(4, cpus), cpus]))
    if any(t < 1 or t > cpus for t in threads):
        raise ValueError('--threads must fit the current CPU affinity mask')
    if any(q < 1 or q > 256 for q in a.qds):
        raise ValueError('--qds must be 1..256')
    if len(a.data_size) != 1 or ('data' in modes and a.data_size[0] < max(a.qds) * 65536):
        raise ValueError('--data-size must be one value, holding one 64 KiB fragment per data QD slot')
    if any(w != 'auto' and w > 64 for w in a.writers):
        raise ValueError('--writers must be 0..64')
    if len(a.store_size) != 1 or a.store_size[0] < 131072 or a.store_size[0] % 131072:
        raise ValueError('--store-size must be one multiple of the 128 KiB logical chunk')
    if a.chunk_base < 0 or a.chunk_base + a.store_size[0] // 131072 >= 2**32:
        raise ValueError('remote working set exceeds chunk-index range')
    duration = str(a.time)
    cases = []

    def add(name, mode, args, **kw):
        cases.append(dict(name=name, mode=mode, args=args, **kw))

    if any(n < 4096 or n > 16 * 1024 * 1024 or n % 4096 for n in a.io_sizes):
        raise ValueError('--io-sizes must be 4 KiB aligned, 4 KiB..16 MiB')
    if any(m in modes for m in ['io', 'io-blake3']) and a.data_size[0] < max(a.io_sizes) * max(a.qds):
        raise ValueError('--data-size must hold one largest I/O block per QD slot')
    for mode in ['io', 'io-blake3']:
        if mode not in modes:
            continue
        for size, qd, direct, rw in itertools.product(a.io_sizes, a.qds, [False, True], ['write', 'read']):
            layouts = ['populated_read'] if rw == 'read' else (['allocating', 'preallocated'] if a.io_write_layouts == 'both' else [a.io_write_layouts])
            for layout in layouts:
                suffix = '-preallocated' if layout == 'preallocated' else ''
                add(f'{mode}-{rw}-{size}-qd{qd}-{"direct" if direct else "buffered"}{suffix}', mode,
                    ['--bench', mode, '--rw', rw, '--io-size', str(size), '--qd', str(qd),
                     '--window', str(a.data_size[0] // (size * qd)), '--time', duration,
                     '--direct-io' if direct else '--no-direct-io'] + (['--preallocate'] if layout == 'preallocated' else []) + (['--sync'] if a.io_sync and rw == 'write' else []),
                    allocation=layout)
    if 'principles' in modes:
        if not 1 <= a.principles_objects <= 1048576 or a.principles_objects < max(a.qds):
            raise ValueError('--principles-objects must be 1..1048576 and at least the largest QD')
        if not 1 <= a.principles_batch <= 1048576:
            raise ValueError('--principles-batch must be 1..1048576')
        if any(n < 4096 or n > 1048576 or n % 4096 for n in a.principles_sizes):
            raise ValueError('--principles-sizes must be 4 KiB aligned, 4 KiB..1 MiB')
        for size, qd, direct, layout in itertools.product(a.principles_sizes, a.qds, [False, True], ['files', 'container']):
            variants = [('read', 'populated', 'none')]
            variants += [('write', policy, persist) for policy in ['allocating', 'overwrite'] for persist in ['none', 'each', 'batch']]
            variants += [('delete', policy, persist) for policy in (['unlink'] if layout == 'files' else ['punch', 'release']) for persist in ['none', 'each', 'batch']]
            for rw, policy, persist in variants:
                add(f'principles-{layout}-{rw}-{policy}-{persist}-{size}-qd{qd}-{"direct" if direct else "buffered"}', 'principles',
                    ['--bench', 'principles', '--layout', layout, '--rw', rw, '--policy', policy,
                     '--persist', persist, '--payload-size', str(size), '--objects', str(a.principles_objects),
                     '--qd', str(qd), '--batch-size', str(a.principles_batch),
                     '--direct-io' if direct else '--no-direct-io'], allocation=policy)
    if 'blake3' in modes:
        for size, nt, style in itertools.product(a.hash_sizes, threads, ['oneshot', 'stream']):
            add(f'blake3-{style}-{size}-t{nt}', 'blake3',
                ['--bench', 'blake3', '--time', duration, '--size', str(size), '--threads', str(nt), f'--{style}'])
    if 'data' in modes:
        phases = ['write', 'read'] if a.data_rw == 'split' else [a.data_rw]
        for direct, writer, qd, rw in itertools.product([False, True], a.writers, a.qds, phases):
            suffix = ('' if rw == 'both' else f'-{rw}') + ('-sync' if a.data_sync else '')
            add(f'data-{"direct" if direct else "buffered"}-w{writer}-qd{qd}{suffix}', 'data',
                ['--bench', 'data', '--time', duration, '--qd', str(qd), '--window', str(a.data_size[0] // (65536 * qd)),
                 '--rw', rw, '--direct-io' if direct else '--no-direct-io'] +
                ([] if writer == 'auto' else ['--writers', str(writer)]) +
                (['--full-paths'] if a.data_full_paths else []) +
                (['--writer-stats'] if a.writer_stats else []) +
                (['--sync'] if a.data_sync else []))
    if 'meta' in modes:
        add('meta-local', 'meta', ['--bench', 'meta', '--time', duration])
    # A fixed-size PUT primes the entire read window; no reads of uninitialized
    # chunks, and timed PUTs wrap inside this bounded working set.
    chunks = a.store_size[0] // 131072
    common = [a.seed, '--id', str(a.chunk_base)] if a.seed else []
    if 'net' in modes:
        add('net', 'net', [a.seed, '--time', duration])
    if 'store' in modes or 'read' in modes:
        add('store-prime', 'store', common + ['--size', str(a.store_size[0])], prerequisite=True)
    if 'store' in modes:
        add('store-timed', 'store', common + ['--store', '--time', duration, '--window', str(chunks)])
    if 'read' in modes:
        add('read', 'read', common + ['--read', '--time', duration, '--window', str(chunks)])
    if 'remote-meta' in modes:
        add('meta-remote', 'remote-meta', common + ['--meta', '--export', a.export,
            '--files', str(a.files), '--dirs', str(a.dirs), '--phases', 'all'])
    # Independent device baselines plus the engine's existing prefix ladder.
    # Metadata has one KV root: compare it on each location, never claim striping.
    if len(a.storage_root) > 1:
        expanded = []
        for case in cases:
            if case['mode'] not in ['io', 'io-blake3', 'principles', 'data', 'meta']:
                expanded.append(case); continue
            groups = [(f'root{i+1}', [i]) for i in range(len(a.storage_root))]
            combined = case['mode'] != 'meta'
            if case['mode'] in ['io', 'io-blake3', 'principles']:
                qd = int(case['args'][case['args'].index('--qd') + 1])
                combined = qd >= len(a.storage_root)
            if combined:
                groups.append((f'roots{len(a.storage_root)}', list(range(len(a.storage_root)))))
            for label, indices in groups:
                expanded.append(dict(case, name=case['name'] + '-' + label,
                                     storage_label=label, root_indices=indices))
        cases = expanded
    return cases


def execute(cmd, directory, stem, timeout, env=None):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / f'{stem}.command.json').write_text(json.dumps(cmd, indent=2) + '\n')
    start = time.monotonic()
    with (directory / f'{stem}.stdout').open('w') as out, (directory / f'{stem}.stderr').open('w') as err:
        proc = subprocess.Popen(cmd, stdout=out, stderr=err, env=env, start_new_session=True)
        try:
            rc = proc.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            # Signal the owned process group, including perf/strace and workers.
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
            raise
    return dict(returncode=rc, elapsed_s=time.monotonic() - start)


def metric_rows(path):
    rows = []
    for line in path.read_text(errors='replace').splitlines():
        if line.startswith(('BENCH_OK ', 'BENCH_FAIL ')):
            row = dict(re.findall(r'(\w+)=([^\s]+)', line))
            if line.startswith('BENCH_FAIL '):
                row['result'] = 'FAIL'
            rows.append(row)
    return rows


def valid_metrics(rows):
    if not rows:
        return False
    for r in rows:
        if r.get('latency_valid', '1') != '1':
            return False
        if 'lat_samples' in r and int(r['lat_samples']) != int(r.get('ops', 0)) + int(r.get('errors', 0)):
            return False
        if r.get('result') == 'FAIL':
            return False
        if any(int(r.get(k, '0')) != 0 for k in ['errors', 'chunks_fail', 'idle_workers']):
            return False
        if 'ops' in r and int(r['ops']) == 0:
            return False
        if 'chunks_ok' in r and int(r['chunks_ok']) == 0:
            return False
        if r.get('kind') == 'blake3' and int(r.get('hashes', '0')) == 0:
            return False
    return True


def profile_rows(path):
    out = []
    if not path.exists():
        return out
    for line in path.read_text(errors='replace').splitlines():
        fields = [s.strip() for s in line.split(';')]
        if len(fields) < 3:
            continue
        try:
            share = float(fields[0].rstrip('%'))
        except ValueError:
            continue
        out.append(dict(percent=share, dso=fields[1], symbol=re.sub(r'^\[[^]]+\]\s*', '', fields[2])))
    return out


def reports(perf, directory, a):
    base = [perf, 'report', '-i', str(directory / 'perf.data'), '--stdio', '--comms', 'efs-bench']
    report_options = {
        'flat': ['--no-children', '--sort', 'dso,symbol', '--percent-limit', '0.3', '-g', 'none'],
        'by-thread': ['--no-children', '--sort', 'comm,pid,symbol', '--percent-limit', '0.5', '-g', 'none'],
        'callers': ['--children', '--sort', 'symbol', '--percent-limit', '2', '-g', 'caller,0.5,callee,function,percent'],
        'symbols': ['--no-children', '--sort', 'dso,symbol', '--percent-limit', '0.3', '-g', 'none', '--field-separator', ';'],
    }
    good = True
    for name, options in report_options.items():
        result = execute(base + options, directory, name, a.timeout)
        good &= result['returncode'] == 0
        if name in ['flat', 'by-thread', 'callers']:
            shutil.copyfile(directory / f'{name}.stdout', directory / {'flat': 'flat.txt', 'by-thread': 'by_thread.txt', 'callers': 'callers.txt'}[name])
    symbols = profile_rows(directory / 'symbols.stdout')
    good &= bool(symbols)
    # Annotate up to three hottest benchmark symbols with C source + assembly.
    # Missing source/debug info is retained as an explicit annotation failure.
    annotated = []
    for row in symbols:
        if 'efs-bench' not in row['dso'] or row['symbol'].startswith('0x'):
            continue
        name = f'annotate-{len(annotated) + 1}'
        result = execute([perf, 'annotate', '-i', str(directory / 'perf.data'), '--stdio', '--source',
                          '--symbol', row['symbol']], directory, name, a.timeout)
        annotated.append(dict(symbol=row['symbol'], **result))
        if len(annotated) == 3:
            break
    (directory / 'annotations.json').write_text(json.dumps(annotated, indent=2) + '\n')
    good &= all(r['returncode'] == 0 for r in annotated)
    return good, symbols


def report_all(perf, output, results, a):
    candidates = [(r, output / r['name'] / 'perf') for r in results if 'perf' in r.get('runs', {})]
    if not candidates:
        raise ValueError('no perf runs found for report generation')
    targets = []
    missing = False
    for r, directory in candidates:
        if (directory / 'perf.data').is_file():
            targets.append((r, directory))
            continue
        missing = True
        r['runs']['perf'].update(reports_valid=False, reports_state='FAILED', reports_error='perf.data missing')
        if r['status'] in ['PASS', 'REPORTS_PENDING']:
            r['status'] = 'FAIL (reports)'
        print(f"Reports unavailable: {r['name']}: perf.data missing", flush=True)
    save_results(output, results)
    print(f"Generating reports for {len(targets)} profiles, up to {a.report_jobs} parallel jobs", flush=True)
    with ThreadPoolExecutor(max_workers=a.report_jobs) as pool:
        pending = {pool.submit(reports, perf, directory, a): r for r, directory in targets}
        for completed, future in enumerate(as_completed(pending), 1):
            r = pending[future]
            try:
                good, _ = future.result()
            except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
                good = False
                r['runs']['perf']['reports_error'] = str(exc)
            else:
                r['runs']['perf'].pop('reports_error', None)
            r['runs']['perf']['reports_valid'] = good
            r['runs']['perf']['reports_state'] = 'COMPLETE' if good else 'FAILED'
            if not good and r['status'] in ['PASS', 'REPORTS_PENDING']:
                r['status'] = 'FAIL (reports)'
            elif good and r['status'] in ['FAIL (reports)', 'REPORTS_PENDING']:
                r['status'] = 'PASS'
            print(f"[reports {completed}/{len(targets)}] {r['name']}: {'OK' if good else 'FAIL'}", flush=True)
            save_results(output, results)
    return not missing and all(r['runs']['perf']['reports_valid'] for r, _ in targets)


def category(row):
    s = row['symbol'].lower()
    if row['dso'] == '[vdso]' or 'clock_gettime' in s:
        return 'time/vDSO'
    if s.startswith('0x') or row['dso'] == '[unknown]':
        return 'unresolved samples'
    if 'blake3' in s or 'efs_hash' in s:
        return 'BLAKE3'
    if any(k in s for k in ['memcpy', 'memmove', 'memset', '_copy_to_iter', 'copy_page_to_iter', 'copy_page_from_iter', 'copy_user']):
        return 'memory copies/fills'
    if any(k in s for k in ['iommu_', 'gup_', 'try_grab_folio', 'try_get_folio', 'bio_set_pages_dirty']):
        return 'page pinning/IOMMU'
    if any(k in s for k in ['futex', 'pthread', 'sched_', 'mutex', '_raw_spin_', 'update_load_avg', 'dequeue_entity', 'enqueue_entity', 'update_curr']):
        return 'synchronization/scheduling'
    if any(k in s for k in ['tcp_', 'recv', 'send', 'efs_conn', 'rdma', 'ibv_']):
        return 'network'
    if any(k in s for k in ['fsync', 'fdatasync', 'pwrite', 'pread', 'writev', 'xfs_', 'ext4_', 'blk_', 'nvme']):
        return 'storage I/O/filesystem'
    return 'other'


def baseline_valid(result):
    baseline = result.get('runs', {}).get('baseline', {})
    return (baseline.get('returncode') == 0 and baseline.get('metrics_valid') is True
            and valid_metrics(result.get('metrics', [])))


def analyze(output):
    manifest = json.loads((output / 'manifest.json').read_text())
    results_path = output / 'results.json'
    results = json.loads(results_path.read_text()) if results_path.exists() else []
    text = ['# EFS benchmark analysis', '', f"Host: `{manifest['host']}`. Event: `{manifest['event']}`.",
            f"Binary SHA256: `{manifest['binary_sha256']}`. Commit: `{manifest['commit']}`.", '',
            'Throughput comes from untraced baseline runs. Perf is a separate rerun; optional strace is another rerun.',
            'CPU sample shares show where execution was sampled, not wall-time spent waiting or proof of an I/O bottleneck.',
            'Profiles filter to efs-bench threads; fio ceiling child processes are excluded. Source annotations need matching debug/source files.',
            'Raw I/O profiles capture only the parallel timed loop; setup, read population and post-run validation are excluded by perf-control acknowledgement.',
            'Engine bounded-write windows are populated before timing. Store-call latency includes admission, scheduling and I/O; p99 is a bounded histogram estimate and max is an exact observed duration. Runs below 10,000 samples are labelled low_samples; maxima are not guarantees.',
            'Data profiles follow --data-rw: both combines phases, split/read/write isolate them. The path-count ladder is omitted with --data-full-paths.', '',
            '| Case | Result | Baseline measurements | Hottest sampled symbols |', '|---|---|---|---|']
    failures = [r for r in results if r['status'] not in ['PASS', 'REPORTS_PENDING']]
    if failures:
        text[4:4] = ['', f"**{len(failures)}/{len(results)} cases failed. Invalid baselines are excluded from comparisons; raw failed output is retained.**", '']
    storage_info = [f"Storage root `{d['path']}`: filesystem device `{d['device_id']}`, initial free bytes {d['free_bytes']}." for d in manifest.get('storage_devices', [])]
    if storage_info:
        text[-2:-2] = storage_info + ['']
    effective_events = set()
    done = set()
    details = []
    for r in results:
        done.add(r['name'])
        measurements = []
        for m in r.get('metrics', []):
            tag = '/'.join(m[k] for k in ['phase', 'rw', 'layout', 'policy', 'persist', 'paths', 'qd', 'allocation', 'sync', 'write_mode'] if k in m)
            numbers = ' '.join(f'{k}={m[k]}' for k in ['GiB_s', 'logical_GiB_s', 'ops_s', 'p50_us', 'p99_lower_us', 'p99_us', 'avg_us', 'max_us', 'lat_samples', 'latency_quality', 'reclaimed_allocated_bytes', 'syncs', 'verified_objects'] if k in m)
            if baseline_valid(r) and numbers:
                measurements.append(f'{tag} {numbers}'.strip())
            elif not baseline_valid(r):
                measurements.append(f"INVALID baseline: errors={m.get('errors', '?')}, idle_workers={m.get('idle_workers', 'not recorded')}")
        cpu_path = output / 'cpu.stdout'
        match = re.search(r'(\d+) bits virtual', cpu_path.read_text(errors='replace')) if cpu_path.exists() else None
        bits = int(match[1]) if match else None
        callers_path = output / r['name'] / 'perf' / 'callers.txt'
        if not callers_path.exists():
            callers_path = callers_path.with_name('callers.stdout')
        bad_frames = bad_callchain_addresses(callers_path, bits)
        if bad_frames:
            details.append(f"**{r['name']}: unreliable call chains — noncanonical caller addresses for this {bits}-bit virtual address space ({', '.join(bad_frames[:3])}).** Frame-pointer unwinding through libc can read data as return addresses. Use flat self samples; re-record engine data with `--call-graph dwarf` (the new auto default for data). DWARF can still truncate deep stacks.")
        symbols_path = output / r['name'] / 'perf' / 'symbols.stdout'
        symbols = profile_rows(symbols_path)
        if symbols_path.exists():
            effective_events.update(re.findall(r"event '([^']+)'", symbols_path.read_text(errors='replace')))
        report_state = r.get('runs', {}).get('perf', {}).get('reports_state')
        if report_state == 'PENDING':
            details.append(f"{r['name']}: reports pending; generated after workloads finish.")
        if r.get('runs', {}).get('perf', {}).get('metrics_valid') is False and symbols:
            details.append(f"**{r['name']}: profile is from an invalid workload rerun; use for diagnosis, not representative hot-path comparisons.**")
        perf_run = r.get('runs', {}).get('perf', {})
        profile_valid = (perf_run.get('metrics_valid', True) and perf_run.get('returncode', 0) == 0
                         and perf_run.get('reports_valid', True))
        hot = ', '.join(f"{s['symbol']} ({s['percent']:.1f}%)" for s in symbols[:3]) or 'no CPU profile'
        if symbols and not profile_valid:
            hot = 'INVALID profile; diagnostic only: ' + hot
        text.append(f"| [{r['name']}]({r['name']}/baseline.stdout) | {r['status']} | {'; '.join(measurements)} | {hot.replace('|', '/')} |")
        if 'strace' in r.get('runs', {}):
            details.append(f"**{r['name']} syscall trace:** [summary]({r['name']}/strace/summary.txt); this separate run uses wall-clock syscall time (-w) and includes setup, population, cleanup and ptrace overhead; it is not a timed-phase latency measurement.")
        annotations = output / r['name'] / 'perf' / 'annotations.json'
        if annotations.exists():
            for annotation in json.loads(annotations.read_text()):
                if annotation['returncode']:
                    details.append(f"Source annotation failed for `{annotation['symbol']}`; inspect annotation stderr.")
        if symbols:
            totals = {}
            for s in symbols:
                key = category(s)
                totals[key] = totals.get(key, 0) + s['percent']
            unresolved = sum(s['percent'] for s in symbols if s['symbol'].startswith('0x') or s['dso'] == '[unknown]')
            if unresolved >= 20:
                details.append(f"**{r['name']}: {unresolved:.1f}% of reported samples have unresolved symbols.** Do not assign those addresses to a storage or algorithm hot path.")
            details += ['', f"**{r['name']} {'sampled CPU' if profile_valid else 'INVALID workload/report — diagnostic CPU samples'}:** " + ', '.join(f'{k} {v:.1f}%' for k, v in sorted(totals.items(), key=lambda x: -x[1])),
                     f"[Flat]({r['name']}/perf/{'flat.txt' if (output / r['name'] / 'perf' / 'flat.txt').exists() else 'flat.stdout'}), [threads]({r['name']}/perf/{'by_thread.txt' if (output / r['name'] / 'perf' / 'by_thread.txt').exists() else 'by-thread.stdout'}), [call chains]({r['name']}/perf/{'callers.txt' if (output / r['name'] / 'perf' / 'callers.txt').exists() else 'callers.stdout'}), source annotations under `{r['name']}/perf/annotate-*.stdout`.", '']
    text += ['', '## Baseline comparisons', '']
    hash_best = {}
    data_best = {}
    for r in results:
        if not baseline_valid(r):
            continue
        for m in r.get('metrics', []):
            if m.get('kind') == 'blake3' and 'GiB_s' in m:
                key = (m.get('mode'), m.get('size'))
                if key not in hash_best or float(m['GiB_s']) > float(hash_best[key][1]['GiB_s']):
                    hash_best[key] = (r['name'], m)
            if m.get('kind') in ['data', 'io'] and 'GiB_s' in m:
                key = ('direct' if 'direct' in r['name'] else 'buffered', m.get('rw'), m.get('paths'), m.get('checksum', 'engine'), m.get('io_bytes', '65536'), m.get('allocation', 'legacy'), r.get('storage_label', 'default'))
                if key not in data_best or float(m['GiB_s']) > float(data_best[key][1]['GiB_s']):
                    data_best[key] = (r['name'], m)
    for key, (name, m) in hash_best.items():
        text.append(f"- BLAKE3 {key[0]}, {key[1]} bytes: best observed {m['GiB_s']} GiB/s at {m.get('threads')} thread(s), `{name}`.")
    for key, (name, m) in data_best.items():
        latency = f"p99 {m['p99_us']} us" if 'p99_us' in m else f"avg/max {m.get('avg_us', '?')}/{m.get('max_us', '?')} us"
        text.append(f"- {key[0]} {key[1]}, {key[2]} path(s), {key[3]} checksum, {key[4]} bytes, {key[5]}, {key[6]}: best observed {m['GiB_s']} GiB/s, {latency}, `{name}`.")
    text += ['', 'These are best observations within this run, not production configuration recommendations.', '', '## CPU hot paths', '']
    text += details
    missing = [c['name'] for c in manifest['cases'] if c['name'] not in done]
    if missing:
        text += ['', 'Uncompleted cases: ' + ', '.join(missing)]
    if not manifest.get('seed'):
        text += ['', 'Cluster net/store/read/metadata modes were not requested: no seed supplied.']
    if effective_events:
        text += ['', 'Recorded perf events: ' + ', '.join(f'`{e}`' for e in sorted(effective_events)) + '. Requested events can be narrowed by host permissions.']
    text += ['', 'Raw buffered I/O is a warm bounded working-set measurement, not physical-device throughput. High QD means more synchronous workers, not asynchronous queue slots.',
             'Raw operation-cycle latency includes benchmark loop bookkeeping and scheduling; compare throughput and latency together. Idle workers invalidate a case; max_start_us records release-to-first-operation delay.',
             'Unresolved kernel/vDSO addresses are retained, not guessed. Time/vDSO and unresolved samples are separate categories.',
             'Category totals are heuristic, mutually exclusive self-sample classifications above the 0.3% report threshold; inspect symbols/callers before choosing changes.',
             'BLAKE3 reuses per-worker buffers: this measures warm-buffer CPU throughput, not disk bandwidth. Streaming omits per-buffer finalize overhead.',
             'Raw allocating writes fill the window once then overwrite it: allocation_ops and overwrite_ops distinguish first fill from warm overwrites. They are not an append-only allocation ceiling.',
             'Local data writes wrap within the configured bounded working set after filling it; profiles include create and replacement work. Direct local CLI without --window retains append-only writes.',
             'Storage-principles cases are independent finite one-pass models, not production GC. Delete GiB_s counts payload represented by removed objects, not transferred bytes; p99 for release is per-container. Required syncs are timed for each/batch; none is acceptance only.',
             'Buffered storage reads are warm; compare direct-I/O cases and the separate ceiling probe before inferring device limits.', '']
    (output / 'ANALYSIS.md').write_text('\n'.join(text))
    return output / 'ANALYSIS.md'


def save_results(output, results):
    tmp = output / 'results.json.tmp'
    tmp.write_text(json.dumps(results, indent=2) + '\n')
    tmp.replace(output / 'results.json')


def main(argv=None):
    a = parser().parse_args(argv)
    if a.report_jobs < 1 or not math.isfinite(a.timeout) or a.timeout <= 0:
        raise ValueError('--report-jobs and --timeout must be positive')
    if a.reports_only:
        if sys.platform != 'linux':
            raise ValueError('regenerate perf reports on the Linux recording host')
        perf = shutil.which('perf')
        if not perf:
            raise ValueError('perf is missing')
        output = a.reports_only.resolve()
        # Fail before launching workers when the recording user's files are
        # inaccessible. Do not partially overwrite another user's reports.
        results_path = output / 'results.json'
        if not os.access(output, os.W_OK) or not os.access(results_path, os.W_OK):
            raise ValueError('run --reports-only as the recording user; result directory is not writable')
        results = json.loads(results_path.read_text())
        for r in results:
            d = output / r['name'] / 'perf'
            if 'perf' in r.get('runs', {}) and d.exists() and (not os.access(d, os.W_OK) or not os.access(d / 'perf.data', os.R_OK)):
                raise ValueError(f"run --reports-only as the recording user; profile is inaccessible: {d}")
        good = report_all(perf, output, results, a)
        print(f'Results and analysis: {analyze(output)}')
        return 0 if good else 1
    if a.analyze:
        print(analyze(a.analyze.resolve()))
        return 0
    if not math.isfinite(a.time) or a.time <= 0 or not math.isfinite(a.timeout) or a.timeout <= 0:
        raise ValueError('--time and --timeout must be finite and positive')
    if a.frequency < 1 or a.files < 1 or a.dirs < 1:
        raise ValueError('frequency/files/dirs must be positive')
    cpus = len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else os.cpu_count() or 1
    cases = cases_for(a, cpus)
    binary = a.binary.resolve()
    if a.dry_run:
        for c in cases:
            print(c['name'] + ': ' + shlex.join([str(binary)] + c['args']) + (' <fresh scratch roots>' if c['mode'] in ['io', 'io-blake3', 'principles', 'data', 'meta'] else ''))
        print(f'{len(cases)} cases: baseline + ' + ('no perf' if a.no_perf else 'perf') + (' + separate strace' if a.strace else ''))
        return 0
    if sys.platform != 'linux':
        raise ValueError('execute profiling on the Linux benchmark host; --dry-run works here')
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError(f'build the benchmark first: make efs-bench ({binary})')
    perf = shutil.which('perf')
    strace = shutil.which('strace')
    if not a.no_perf and not perf:
        raise ValueError('perf is missing; install the host perf package or explicitly use --no-perf')
    if a.strace and not strace:
        raise ValueError('strace is missing')
    roots = [p.resolve() for p in a.storage_root]
    if any(not p.is_dir() for p in roots):
        raise ValueError('--storage-root parents must already exist')
    if len(set(roots)) != len(roots):
        raise ValueError('storage roots must resolve to distinct directories')
    storage_devices = [dict(path=str(p), device_id=f'{os.major(p.stat().st_dev)}:{os.minor(p.stat().st_dev)}', free_bytes=shutil.disk_usage(p).free) for p in roots]
    if a.require_distinct_devices and (len(roots) < 2 or len({d['device_id'] for d in storage_devices}) != len(roots)):
        raise ValueError('--require-distinct-devices needs at least two roots on distinct filesystem devices')
    if len(roots) < 2:
        print('Storage coverage: one location; supply --storage-root twice for multi-location comparisons', flush=True)
    elif len({d['device_id'] for d in storage_devices}) < len(roots):
        print('Storage coverage: some roots share a filesystem device; directory scaling is not independent-device scaling', flush=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    output = (a.output or ROOT / 'logs' / f'efs-bench-{stamp}').resolve()
    output.mkdir(parents=True, exist_ok=False)
    # Keep a matching executable with profiles even after a later rebuild.
    shutil.copy2(binary, output / 'efs-bench')
    with zipfile.ZipFile(output / 'sources.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for parent in ['src', 'include', 'deps/blake3']:
            for source in sorted((ROOT / parent).rglob('*')):
                if source.suffix in ['.c', '.h', '.S', '.s']:
                    archive.write(source, source.relative_to(ROOT))
        archive.write(ROOT / 'Makefile', 'Makefile')
    profiled_binary = str(output / 'efs-bench')
    commit = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip() or 'unknown'
    manifest = dict(host=os.uname().nodename, event='none' if a.no_perf else a.event,
                    binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), commit=commit,
                    seed=a.seed, created_utc=stamp, options={k: str(v) if isinstance(v, Path) else v for k, v in vars(a).items()}, cases=cases)
    manifest['storage_devices'] = storage_devices
    manifest['options']['storage_root'] = [str(p) for p in roots]
    manifest['affinity_cpus'] = sorted(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else None
    manifest['perf_policy'] = {}
    for name in ['perf_event_paranoid', 'kptr_restrict']:
        try:
            manifest['perf_policy'][name] = Path('/proc/sys/kernel', name).read_text().strip()
        except OSError:
            manifest['perf_policy'][name] = 'unavailable'
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    results = []
    try:
        execute([profiled_binary, '--version'], output, 'version', a.timeout)
        if perf and not a.no_perf:
            execute([perf, 'version'], output, 'perf-version', a.timeout)
        for name, cmd in [('uname', ['uname', '-a']), ('cpu', ['lscpu']), ('memory', ['free', '-h']),
                          ('mounts', ['findmnt', '-T', str(output)]), ('devices', ['lsblk', '-o', 'NAME,SIZE,TYPE,FSTYPE,MOUNTPOINTS'])]:
            if shutil.which(cmd[0]):
                execute(cmd, output, name, a.timeout)
        if shutil.which('findmnt'):
            for i, root in enumerate(roots):
                execute(['findmnt', '-T', str(root)], output, f'storage-mount-{i + 1}', a.timeout)
        if not a.no_perf:
            preflight = execute([perf, 'record', '-e', a.event, '-F', str(a.frequency), '-g', '--call-graph', call_graph_for(a, 'preflight'),
                                 '-o', str(output / 'preflight.data'), '--', sys.executable, '-c', 'sum(range(1000000))'],
                                output, 'perf-preflight', a.timeout)
            if preflight['returncode']:
                raise ValueError(f'perf cannot record event {a.event}; see {output}/perf-preflight.stderr (try --event cpu-clock for a missing PMU)')
        ceiling_done = set()
        prime_ok = True
        for index, case in enumerate(cases, 1):
            print(f"[{index}/{len(cases)}] {case['name']}", flush=True)
            directory = output / case['name']
            directory.mkdir()
            result = dict(name=case['name'], status='RUNNING', runs={}, storage_label=case.get('storage_label', 'default'), root_indices=case.get('root_indices', list(range(len(roots)))))
            results.append(result)
            save_results(output, results)
            if case['mode'] == 'read' and not prime_ok:
                result['status'] = 'SKIP (store prime failed)'
                save_results(output, results)
                continue
            for stage in ['baseline'] + ([] if a.no_perf else ['perf']) + (['strace'] if a.strace else []):
                scratch = []
                cmd = [profiled_binary] + case['args']
                if case['mode'] in ['io', 'io-blake3', 'principles', 'data', 'meta']:
                    parents = [roots[i] for i in case['root_indices']] if 'root_indices' in case else roots or [output / 'scratch']
                    for parent in parents:
                        parent.mkdir(exist_ok=True)
                        scratch.append(Path(tempfile.mkdtemp(prefix='efs-bench-', dir=parent)))
                    if case['mode'] in ['io', 'io-blake3', 'principles', 'data']:
                        for root in scratch:
                            cmd += ['--storage', str(root)]
                    else:
                        cmd += ['--meta-storage', str(scratch[0])]
                    # One unprofiled same-host ceiling probe only; no fio cost
                    # or samples mixed into engine CPU profiles.
                    if case['mode'] in ['io', 'io-blake3', 'principles']:
                        pass
                    elif a.skip_ceiling or tuple(case.get('root_indices', range(len(roots)))) in ceiling_done or stage != 'baseline':
                        cmd += ['--skip-ceiling']
                    else:
                        ceiling_done.add(tuple(case.get('root_indices', range(len(roots)))))
                stage_dir = directory if stage == 'baseline' else directory / stage
                workload_cmd = cmd[:]
                control_dir = None
                control = ack = None
                if stage == 'perf' and case['mode'] in ['io', 'io-blake3', 'principles', 'data']:
                    control_dir = tempfile.TemporaryDirectory(prefix='efs-bench-perf-control-')
                    control, ack = str(Path(control_dir.name) / 'control'), str(Path(control_dir.name) / 'ack')
                    os.mkfifo(control); os.mkfifo(ack)
                if stage == 'perf':
                    cmd = [perf, 'record', '-e', a.event, '-F', str(a.frequency), '-g', '--call-graph', call_graph_for(a, case['mode']),
                           '-o', str(stage_dir / 'perf.data')] + (['--delay=-1', '--control', f'fifo:{control},{ack}'] if control else []) + ['--'] + cmd
                elif stage == 'strace':
                    cmd = [strace, '-f', '-c', '-w', '-o', str(stage_dir / 'summary.txt')] + (['-e', a.strace_expr] if a.strace_expr else []) + cmd
                # Prevent inherited EFS_PERF_PATH from causing nested profiling.
                env = os.environ.copy()
                env.pop('EFS_PERF_PATH', None)
                env.pop('EFS_BENCH_PERF_CONTROL', None); env.pop('EFS_BENCH_PERF_ACK', None)
                if control:
                    env['EFS_BENCH_PERF_CONTROL'] = control; env['EFS_BENCH_PERF_ACK'] = ack
                try:
                    run = execute(cmd, stage_dir, stage, a.timeout, env)
                    rows = metric_rows(stage_dir / f'{stage}.stdout')
                    run['metrics_valid'] = valid_metrics(rows)
                    run['command'] = workload_cmd
                    result['runs'][stage] = run
                    if stage == 'baseline':
                        result['metrics'] = rows
                    if stage == 'perf':
                        run['reports_state'] = 'PENDING'
                        run['call_graph'] = call_graph_for(a, case['mode'])
                    if run['returncode'] != 0 or not run['metrics_valid'] or run.get('reports_valid') is False:
                        result['status'] = f'FAIL ({stage})'
                        break
                finally:
                    # Delete only the private children created by this process.
                    # Caller-provided storage roots are never cleared.
                    for root in scratch:
                        shutil.rmtree(root)
                    if control_dir is not None:
                        control_dir.cleanup()
            if result['status'] == 'RUNNING':
                result['status'] = 'REPORTS_PENDING' if not a.no_perf else 'PASS'
            if case.get('prerequisite'):
                prime_ok = result['status'] in ['PASS', 'REPORTS_PENDING']
            save_results(output, results)
            analyze(output)
        if not a.no_perf:
            report_all(perf, output, results, a)
        print(f"Results and analysis: {analyze(output)}")
        return 0 if all(r['status'] == 'PASS' for r in results) else 1
    except BaseException as exc:
        if results and results[-1]['status'] == 'RUNNING':
            results[-1]['status'] = 'INTERRUPTED/FAILED'
            results[-1]['error'] = str(exc) or type(exc).__name__
        raise
    finally:
        save_results(output, results)
        analyze(output)


def terminated(signum, frame):
    raise KeyboardInterrupt


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, terminated)
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.TimeoutExpired) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        print('Interrupted; completed evidence and analysis retained.', file=sys.stderr)
        sys.exit(130)
