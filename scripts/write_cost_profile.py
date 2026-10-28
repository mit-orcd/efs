#!/usr/bin/env python3
"""K32 measurement only: matched file-per-request writes, no product changes.

A fresh private 3-member TCP/O_DIRECT cluster per case. The measurement-only
C client and FUSE each create total/io files, one write per file. Optional
bench mode preserves the unmodified benchmark's failure, not a valid baseline.
Includes final cluster data-commit; verifies every byte outside measurement.
strace changes scheduling: timings/CPU describe this instrumented run, not IOR.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import resource
import signal
import struct
import subprocess
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
MIB = 1 << 20
TICKET_BYTES = 216


def read_wal(path):
    """Strict current RFL1 decoder. Zero reservation tail is not log bytes."""
    records = []
    with Path(path).open('rb') as f:
        identity = os.fstat(f.fileno()).st_ino
        assert f.read(8) == struct.pack('>II', 0x52464C31, 1), 'WAL header'
        end = 8
        while True:
            header = f.read(8)
            if not header or header == b'\0' * 8:
                break
            assert len(header) == 8, 'short WAL frame'
            crc, length = struct.unpack('>II', header)
            assert 5 <= length <= (8 << 20) + 64, ('WAL length', length)
            payload = f.read(length)
            assert len(payload) == length and zlib.crc32(payload) == crc, 'WAL CRC/length'
            kind, group = payload[0], int.from_bytes(payload[1:5], 'big')
            row = dict(kind=kind, group=group, start=end, bytes=length + 8)
            if kind == 2:
                assert length >= 25
                index, term, n = struct.unpack('>QQI', payload[5:25])
                assert n == length - 25
                row.update(index=index, term=term, command=payload[25:])
            else:
                assert kind in (1, 3, 4, 5), ('WAL kind', kind)
            records.append(row)
            end += length + 8
        assert os.fstat(f.fileno()).st_ino == identity
    return dict(inode=identity, end=end, records=records)


def flatten(command):
    """Count nested TXN_BATCH commands separately from their enclosing entry."""
    assert command, 'empty command'
    if command[0] != 88:
        return [command]
    assert len(command) >= 4
    count = int.from_bytes(command[2:4], 'big')
    out, off = [], 4
    for _ in range(count):
        assert off + 2 <= len(command)
        n = int.from_bytes(command[off:off + 2], 'big')
        off += 2
        assert n and off + n <= len(command)
        out.extend(flatten(command[off:off + n]))
        off += n
    assert off == len(command), 'TXN_BATCH trailing bytes'
    return out


def wal_cost(before, after, applied_before, applied_after):
    assert before['inode'] == after['inode'], 'log rotated: this window needs rotation accounting'
    assert after['end'] >= before['end']
    appended = [r for r in after['records'] if r['start'] >= before['end']]
    # Status and WAL reads cannot be atomic with background append/apply.
    # Reconcile commands by each group's applied index, not by file offset.
    # Keep physical appended bytes separately, including boundary appends.
    selected = [r for r in after['records'] if r['kind'] == 2 and
                applied_before[r['group']] < r['index'] <= applied_after[r['group']]]
    entries, commands, nested = Counter(), Counter(), Counter()
    admitted, settled = set(), set()
    for row in selected:
        g, i, cmd = row['group'], row['index'], row['command']
        entries[g] += 1
        commands[cmd[0]] += 1
        for c in flatten(cmd):
            nested[c[0]] += 1
            if c[0] == 39:
                assert len(c) == 1 + TICKET_BYTES
                admitted.add(c[1:29].hex())
            elif c[0] in (42, 90, 91):
                assert len(c) >= 3
                n = int.from_bytes(c[1:3], 'big')
                assert n and len(c) == 3 + n * TICKET_BYTES
                target = admitted if c[0] == 90 else settled
                for k in range(n):
                    target.add(c[3 + k * TICKET_BYTES:31 + k * TICKET_BYTES].hex())
    for g in (0, 2):
        assert entries[g] == applied_after[g] - applied_before[g], ('entry reconciliation', g)
        assert {r['index'] for r in selected if r['group'] == g} == set(
            range(applied_before[g] + 1, applied_after[g] + 1)), ('index gap/duplicate', g)
    return dict(entries=dict(entries), top_commands=dict(commands), nested_commands=dict(nested),
                admitted_unique=len(admitted), settle_items_unique=len(settled),
                settle_not_in_admit=len(settled - admitted),
                valid_record_bytes=sum(r['bytes'] for r in appended),
                entry_bytes=sum(r['bytes'] for r in selected),
                boundary_appends_outside_applied=sum(r['kind'] == 2 and
                    not (applied_before[r['group']] < r['index'] <= applied_after[r['group']])
                    for r in appended),
                applied_entries_already_appended=sum(r['start'] < before['end'] for r in selected))


_HEX = re.compile(r'\\x([0-9a-fA-F]{2})')


def parse_trace(text):
    """Successful REPORT wire sends and complete reply frames, per strace thread.

    -ff keeps one thread per file. RPC send/receive remains on its checkout
    thread; 4-byte length read then payload reads never consume another frame.
    Failed EAGAIN and incomplete syscalls do not count as completed frames.
    """
    sends, replies, orphan_reads = 0, 0, 0
    state, pending = {}, None
    for raw in text.splitlines():
        line = re.sub(r'^\d+\.\d+\s+', '', raw)
        if '<unfinished ...>' in line:
            pending = line.split('<unfinished ...>')[0]
            continue
        if line.startswith('<...') and ' resumed>' in line:
            if pending is None:
                continue
            line = pending + line.split(' resumed>', 1)[1]
            pending = None
        ret = re.search(r'\)\s+=\s+(-?\d+)', line)
        if not ret:
            continue
        n = int(ret[1])
        if n < 0:
            continue
        call = re.match(r'(\w+)\((\d+)(?:,|\))', line)
        if not call:
            continue
        op, fd = call[1], int(call[2])
        if op == 'close':
            state.pop(fd, None)
        if op in ('sendmsg', 'writev') and n >= 5:
            first = re.search(r'iov_base="([^"]*)", iov_len=5\b', line)
            if first:
                header = bytes(int(h, 16) for h in _HEX.findall(first[1]))
                if len(header) == 5 and header[4] == 67 and 1 <= int.from_bytes(header[:4], 'big') <= (8 << 20):
                    sends += 1
        if op not in ('read', 'recvfrom') or n == 0:
            continue
        body = re.match(r'\w+\(\d+, "([^"]*)"(?:\.\.\.)?, (\d+)', line)
        if not body:
            continue
        data = bytes(int(h, 16) for h in _HEX.findall(body[1]))
        asked = int(body[2])
        if fd not in state:
            if asked != 4:
                orphan_reads += 1
                continue
            state[fd] = {'header': bytearray(), 'left': None, 'type': None}
        st = state[fd]
        if st['left'] is None:
            assert len(data) == n and len(st['header']) + n <= 4, 'partial frame header'
            st['header'].extend(data)
            if len(st['header']) == 4:
                length = int.from_bytes(st['header'], 'big')
                # Ordinary file reads are not protocol headers.
                if not 1 <= length <= (8 << 20):
                    state.pop(fd)
                    continue
                st['left'] = length
        else:
            assert n <= st['left'], 'read crosses protocol frame'
            if st['type'] is None and data:
                st['type'] = data[0]
            st['left'] -= n
            if st['left'] == 0:
                replies += st['type'] == 68
                state.pop(fd)
    return dict(report_sends=sends, report_replies=replies, orphan_reads=orphan_reads)


def parse_sync_summary(text):
    counts = {'fsync': 0, 'fdatasync': 0}
    for line in text.splitlines():
        parts = line.split()
        if parts and parts[-1] in counts:
            assert len(parts) in (5, 6), ('strace -c shape', line)
            counts[parts[-1]] += int(parts[3])
    return counts


def cpu_seconds(pid):
    fields = Path('/proc/%d/stat' % pid).read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def dump(path, obj):
    Path(path).write_text(json.dumps(obj, indent=2, sort_keys=True) + '\n')


def checked(argv, env=None, timeout=120):
    p = subprocess.run([str(x) for x in argv], env=env, text=True, capture_output=True, timeout=timeout)
    assert p.returncode == 0, (argv, p.returncode, p.stdout[-2000:], p.stderr[-2000:])
    return p.stdout


def attach(pid, output, summaries=False):
    args = ['strace', '-f', '-qq']
    args += ['-c', '-e', 'trace=fsync,fdatasync'] if summaries else ['-ff', '-ttt', '-xx', '-s', '16', '-e', 'trace=sendmsg,writev,read,recvfrom,close']
    log = Path(str(output) + '.attach.log').open('w')
    proc = subprocess.Popen(args + ['-o', str(output), '-p', str(pid)], stdout=log, stderr=log,
                            start_new_session=True)
    end = time.monotonic() + 10
    while time.monotonic() < end:
        assert proc.poll() is None, Path(log.name).read_text()
        if re.search(r'^TracerPid:\s+[1-9]', Path('/proc/%d/status' % pid).read_text(), re.M):
            return proc, log
        time.sleep(0.02)
    raise AssertionError('strace attach timeout')


def detach(pair):
    proc, log = pair
    proc.send_signal(signal.SIGINT)
    proc.wait(timeout=20)
    log.close()
    assert proc.returncode in (0, -signal.SIGINT), ('trace exit', proc.returncode)


def statuses(c, output, label):
    rows = {}
    for n in (1, 2, 3):
        text = checked([ROOT / 'efs-mgmt', 'raft-status', c.ep(n)], env=c.env)
        (output / ('status-%s-n%d.txt' % (label, n))).write_text(text)
        group = {}
        for m in re.finditer(r'^  group (\d+) .*?role=(\w+) leader=(-?\d+) term=(\d+) commit=(\d+) applied=(\d+)', text, re.M):
            group[int(m[1])] = dict(role=m[2], leader=int(m[3]), term=int(m[4]), commit=int(m[5]), applied=int(m[6]))
        assert set(group) == {0, 2}, text
        rows[n] = group
    return rows


def quiet(c, output, label):
    deadline, previous, steady = time.monotonic() + 60, None, 0
    while time.monotonic() < deadline:
        current = statuses(c, output, label)
        view = tuple((n, g, current[n][g]['commit'], current[n][g]['applied']) for n in current for g in current[n])
        caught = all(current[n][g]['commit'] == current[n][g]['applied'] == current[1][g]['applied']
                     for n in current for g in current[n])
        steady = steady + 1 if caught and view == previous else 0
        if steady >= 3:
            return current
        previous = view
        time.sleep(0.2)
    raise AssertionError('no quiescent applied frontier')


def worker(mount, total, io_size, writers):
    files = total // io_size
    payload = b'A' * io_size
    start = time.monotonic()
    def run(w):
        parent = mount / ('writer-%d' % w)
        parent.mkdir()
        for n in range(w, files, writers):
            fd = os.open(parent / ('file-%d' % n), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
            try:
                done = os.write(fd, payload)
                assert done == io_size, ('short write', done, io_size)
            finally:
                os.close(fd)
    with ThreadPoolExecutor(max_workers=writers) as pool:
        list(pool.map(run, range(writers)))
    print(json.dumps(dict(bytes=total, files=files, wall_s=time.monotonic()-start)), flush=True)


def verify(mount, total, io_size):
    count, size = 0, 0
    for parent, dirs, files in os.walk(mount):
        for name in files:
            count += 1
            with open(Path(parent) / name, 'rb', buffering=0) as f:
                n = 0
                while True:
                    block = f.read(MIB)
                    if not block:
                        break
                    assert block == b'A' * len(block), ('payload mismatch', parent, name, n)
                    n += len(block)
                assert n == io_size, ('file size', name, n, io_size)
                size += n
    assert size == total and count == total // io_size, ('namespace/byte count', count, size)
    return dict(files=count, bytes=size, pattern='every byte A; not a version-identity oracle')


def run_case(work, output, mode, io_size, writers, total):
    sys.path.insert(0, str(ROOT / 'tests'))
    import k33_daemons as k
    k.EFSD = ROOT / 'efsd'
    env = {key: val for key, val in os.environ.items() if not key.startswith('EFS_')}
    env.update(EFS_TRANSPORT='tcp', EFS_BATCH_OBS='1', EFS_RAFT_OBS='1', EFS_PUT_TICKET_DBG='1')
    output.mkdir(parents=True, exist_ok=False)
    c = k.Cluster(work, output.name, [1, 2, 3], env)
    mount = None
    tracers = []
    result = dict(mode=mode, io_bytes=io_size, writers=writers, total_bytes=total, verdict='FAIL')
    try:
        c.start(1, quota='8G')
        c.start(2, join=1, quota='8G')
        c.start(3, join=1, quota='8G')
        c.wait_members((1, 2, 3), 3)
        c.wait_voters((1, 2, 3), lambda n, gs: set(gs) == {0, 2} and all(hosted and v == 7 for hosted, v in gs.values()))
        mount = k.Mount(c, 1, 'mnt')
        before = quiet(c, output, 'before')
        wal_before = {n: read_wal(c.dir / ('n%d/mdraft/log/raft.log' % n)) for n in (1, 2, 3)}
        for n in (1, 2, 3):
            tracers.append(attach(c.procs[n].pid, output / ('sync-n%d.txt' % n), True))
        client_trace = None
        if mode == 'fuse':
            client_trace = attach(mount.p.pid, output / 'client-trace')
            tracers.append(client_trace)
        cpu_start = [cpu_seconds(c.procs[n].pid) for n in (1, 2, 3)]
        fuse_start = cpu_seconds(mount.p.pid)
        started = time.monotonic()
        clock = ['/usr/bin/time', '-o', str(output / 'workload.cpu'), '-f', '%U %S']
        if mode in ('bench', 'direct'):
            workload = ([str(work / 'write-cost-client'), c.ep(1), str(total),
                         str(io_size), str(writers)] if mode == 'direct' else
                        [str(ROOT / 'efs-bench'), c.ep(1), '--meta', '--phases', 'create', '--keep',
                         '--size', str(io_size), '--files', str(total // io_size), '--dirs', str(writers),
                         '--workers', str(writers), '--export', 'default'])
            argv = ['strace', '-f', '-ff', '-qq', '-ttt', '-xx', '-s', '16', '-e',
                    'trace=sendmsg,writev,read,recvfrom,close', '-o', str(output / 'client-trace'),
                    *clock, *workload]
        else:
            argv = [*clock, sys.executable, str(Path(__file__).resolve()), '--worker',
                    str(mount.path), str(total), str(io_size), str(writers)]
        (output / 'command.json').write_text(json.dumps(argv) + '\n')
        with (output / 'workload.log').open('w') as log:
            p = subprocess.run(argv, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=900)
        assert p.returncode == 0, (p.returncode, (output / 'workload.log').read_text()[-4000:])
        commit = checked([ROOT / 'efs-mgmt', 'data-commit', *(c.ep(n) for n in (1, 2, 3))], env=env)
        (output / 'data-commit.txt').write_text(commit)
        after = quiet(c, output, 'after')
        wall = time.monotonic() - started
        cpus = [cpu_seconds(c.procs[n].pid)-cpu_start[n-1] for n in (1, 2, 3)]
        fuse_cpu = cpu_seconds(mount.p.pid)-fuse_start
        for pair in reversed(tracers):
            detach(pair)
        tracers.clear()
        wal_after = {n: read_wal(c.dir / ('n%d/mdraft/log/raft.log' % n)) for n in (1, 2, 3)}
        costs = {n: wal_cost(wal_before[n], wal_after[n],
                             {g: before[n][g]['applied'] for g in (0, 2)},
                             {g: after[n][g]['applied'] for g in (0, 2)}) for n in (1, 2, 3)}
        for n in (2, 3):
            assert costs[n]['entries'] == costs[1]['entries'], 'replica entry disagreement'
        reports = Counter()
        for trace in output.glob('client-trace.*'):
            if trace.name.endswith('.log'):
                continue
            reports.update(parse_trace(trace.read_text(errors='replace')))
        client_text = (output / 'workload.log').read_text(errors='replace') if mode != 'fuse' else mount.logp.read_text(errors='replace')
        (output / 'measured-client.log').write_text(client_text)
        settled_ok = 0
        for line in client_text.splitlines():
            m = re.search(r'put-ticket settle .*?retired=(\d+) rc=0\b', line)
            if m:
                settled_ok += int(m[1])
            m = re.search(r'put-ticket settle batch .*?mask=(0x[0-9a-f]+|0) rc=0\b', line)
            if m:
                settled_ok += bin(int(m[1], 0)).count('1')
        assert settled_ok <= costs[1]['admitted_unique'], ('settle accounting', settled_ok, costs[1])
        syncs = [parse_sync_summary((output / ('sync-n%d.txt' % n)).read_text()) for n in (1, 2, 3)]
        workload_cpu = sum(float(x) for x in (output / 'workload.cpu').read_text().split())
        result.update(verdict='PASS', wall_s=wall, daemon_cpu_s=cpus, workload_cpu_s=workload_cpu,
                      client_cpu_s=workload_cpu if mode != 'fuse' else fuse_cpu,
                      client_fuse_cpu_s=fuse_cpu, costs=costs, report=dict(reports), syncs=syncs,
                      tickets_settled_success=settled_ok,
                      before=before, after=after,
                      measurement='create/write/publish plus cluster COMMIT and quiescence; tracing overhead included')
        result['verification'] = verify(mount.path, total, io_size)
        assert reports['report_sends'] > 0 and reports['report_sends'] == reports['report_replies'], dict(reports)
        for n in (1, 2, 3):
            (output / ('io-n%d.txt' % n)).write_text(checked([ROOT / 'efs-mgmt', 'io-stats', c.ep(n)], env=env))
    except BaseException as error:
        result.update(verdict='FAIL', error=repr(error))
        raise
    finally:
        for pair in reversed(tracers):
            detach(pair)
        if mount is not None:
            # This mount is owned and idle; strict stop, no lazy detach or forced discard.
            stop = subprocess.run([str(ROOT / 'scripts/client.sh'), 'stop', str(mount.path)],
                                  env=env, text=True, capture_output=True, timeout=650)
            (output / 'client-stop.txt').write_text(stop.stdout + stop.stderr)
            if stop.returncode != 0 or os.path.ismount(mount.path):
                result.update(verdict='FAIL', cleanup_error='client stop failed')
            if result.get('cleanup_error'):
                dump(output / 'result.json', result)
                dump(output / 'live-fixture.json', {'pids': {n: p.pid for n, p in c.procs.items()}, 'mount': str(mount.path)})
                raise AssertionError('client stop failed; servers preserved for investigation')
            assert mount.p.wait(timeout=30) == 0, 'client did not exit cleanly'
            mount.log.close()
        for n, proc in list(c.procs.items()):
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            rc = proc.wait(timeout=60)
            if rc != 0:
                result.update(verdict='FAIL', cleanup_error='daemon stop rc=%d' % rc)
            record = c.dir / ('n%d/log/efsd.exit' % n)
            (output / ('exit-n%d.txt' % n)).write_text(record.read_text())
            assert 'outcome=orderly' in record.read_text(), record.read_text()
        c.stop_all()
        for log in c.dir.glob('*.log'):
            (output / log.name).write_bytes(log.read_bytes())
        dump(output / 'result.json', result)
    assert result['verdict'] == 'PASS', result
    return result


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--worker':
        worker(Path(sys.argv[2]), *map(int, sys.argv[3:6]))
        return
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--total-mib', type=int, default=1024)
    p.add_argument('--io-mib', type=int, nargs='+', default=[1, 4, 16])
    p.add_argument('--writers', type=int, nargs='+', default=[1, 8])
    p.add_argument('--modes', nargs='+', choices=['direct', 'bench', 'fuse'], default=['direct', 'fuse'])
    args = p.parse_args()
    assert args.total_mib > 0 and all(n in (1, 4, 16) and args.total_mib % n == 0 for n in args.io_mib)
    assert all(n in (1, 8) for n in args.writers)
    assert all(args.total_mib // io >= n for io in args.io_mib for n in args.writers)
    args.work = args.work.resolve()
    args.output = args.output.resolve()
    args.work.mkdir(parents=True, exist_ok=False)
    args.output.mkdir(parents=True, exist_ok=False)
    if 'direct' in args.modes:
        # Build only the measurement executable; product sources are unchanged.
        subprocess.run(['gcc', '-O3', '-g', '-D_GNU_SOURCE', '-std=c99', '-Wall', '-Wextra',
                        '-Iinclude', '-Isrc/common', '-Ideps/blake3',
                        'scripts/write_cost_client.c', 'libefs.a', '-lpthread', '-lm', '-ldl',
                        '-libverbs', '-o', str(args.work / 'write-cost-client')], cwd=ROOT, check=True)
    rows = []
    for mode in args.modes:
        for io in args.io_mib:
            for writers in args.writers:
                name = '%s-io%d-w%d' % (mode, io, writers)
                row = run_case(args.work, args.output / name, mode, io * MIB, writers, args.total_mib * MIB)
                rows.append(row)
                dump(args.output / 'results.json', rows)
                print(name, 'PASS', 'wall_s=%.3f' % row['wall_s'], flush=True)


if __name__ == '__main__':
    main()
