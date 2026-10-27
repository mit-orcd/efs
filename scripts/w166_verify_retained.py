#!/usr/bin/env python3
"""Orderly restart + fresh-client, exact readback of every confirmed W166 close.

Only private diagnostic fixtures beneath $HOME are accepted. Start the same
three stores on their recorded endpoints with the matching build, never format,
read all acknowledged payloads (not samples or failed writes), preserve errors,
and stop only these children. Refuse occupied ports rather than replacing an
existing service. Run after the gate on an otherwise quiet assigned host.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import k33_daemons as k


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', type=Path, required=True)
    p.add_argument('--source', type=Path, required=True, help='matching retained binary tree')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--quota', required=True, help='original explicit per-member fixture quota')
    args = p.parse_args()
    args.run, args.source, args.output = (x.resolve() for x in (args.run, args.source, args.output))
    for path in (args.run, args.source, args.output):
        assert path.is_relative_to(Path.home()) and not str(path).startswith('/data1/'), path
    assert args.output != args.run and not args.output.exists()
    original = json.loads((args.run / 'result.json').read_text())
    assert original.get('quota', args.quota) == args.quota, 'quota differs from the recorded fixture'
    rows = [json.loads(s) for s in (args.run / 'application.jsonl').read_text().splitlines()]
    acked = [(int(r['writer']), int(r['seq'])) for r in rows
             if r.get('stage') == 'close' and r.get('event') == 'ok']
    assert acked and len(set(acked)) == len(acked)
    assert all(0 <= w < 4 and seq > 0 for w, seq in acked)
    begin = next(r for r in rows if r.get('event') == 'pressure-begin')
    ports = {int(i): int(port) for i, port in begin['endpoints'].items()}
    assert set(ports) == {1, 2, 3}
    assert all(1024 <= port <= 65535 and port not in (17432, 17433, 17434, 17435, 27432, 27433)
               for port in ports.values())
    stores = {i: args.run / 'cluster' / f'n{i}' for i in ports}
    assert all((store / 'mdraft' / 'boot.bin').is_file() for store in stores.values()), stores
    k.EFSD = args.source / 'tests/faults/efsd'
    k.MGMT, k.FUSE = args.source / 'efs-mgmt', args.source / 'efs-fuse'
    assert all(x.is_file() for x in (k.EFSD, k.MGMT, k.FUSE))
    version = subprocess.run([str(k.EFSD), '--version'], capture_output=True, text=True, check=True).stdout.strip()
    revision = original.get('source_revision', 'unspecified')
    assert revision != 'unspecified' and version.startswith('efsd ' + revision + ' '), version
    args.output.mkdir(parents=True, exist_ok=False)
    env = {key: value for key, value in os.environ.items() if not key.startswith(('EFS_', 'K22_'))}
    env.update(EFS_TRANSPORT='tcp', EFS_RPC_PROF='1')
    c = k.Cluster.__new__(k.Cluster)
    c.dir, c.env, c.ports = args.output, env, ports
    c.hosts, c.disc_port, c.procs, c.held = {}, 0, {}, []
    mount = None
    start = time.monotonic()
    result = dict(outcome='HARNESS_FAILURE', fixture=str(args.run), quota=args.quota,
                  binary_tree=str(args.source), build=version, expected=len(acked), checked=0, errors=[],
                  binary_sha256={str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in (k.EFSD, k.MGMT, k.FUSE)},
                  method='all daemons restarted, new FUSE process, exact full acknowledged payloads',
                  verifier_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())

    def save():
        result['wall_s'] = time.monotonic() - start
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')

    try:
        for port in ports.values():
            held = k.hold_port(k.HOST, port)
            assert held is not None, f'recorded port occupied: {port}; nothing stopped'
            c.held.append(held)
        assert not (args.run / 'compactor-parked').exists(), 'pressure marker unexpectedly armed'
        c.start(1, store=stores[1], quota=args.quota, extra_env={
            'EFS_FAULT_L0_BYTES': str(2 << 20), 'EFS_FAULT_MEMTABLE_BYTES': str(1 << 20),
            'EFS_FAULT_COMPACT_STALL': '1', 'EFS_FAULT_COMPACT_FILE': str(args.run / 'compactor-parked')})
        for i in (2, 3):
            c.start(i, join=1, store=stores[i], quota=args.quota)
        c.wait_voters([1, 2, 3], lambda i, v: v == {0: (1, 7), 2: (1, 7)}, timeout=120)
        result['leaders'] = c.leader_placement(2)
        mount = k.Mount(c, 2, 'fresh-reader')
        result['ready_s'] = time.monotonic() - start
        for writer, seq in acked:
            want = hashlib.sha256(f'{writer}:{seq}'.encode()).digest() * 64
            name = mount.path / f'writer-{writer}' / str(seq)
            try:
                got = name.read_bytes()
                if got != want:
                    result['errors'].append(dict(writer=writer, seq=seq, kind='mismatch',
                                                 bytes=len(got), sha256=hashlib.sha256(got).hexdigest()))
            except OSError as e:
                result['errors'].append(dict(writer=writer, seq=seq, kind='read-error', errno=e.errno))
            result['checked'] += 1
            if result['checked'] % 500 == 0:
                save()
        result['outcome'] = 'FAIL' if result['errors'] else 'PASS'
    except Exception as e:
        result['harness_error'] = repr(e)
    finally:
        cleanup = []
        if mount is not None:
            try:
                mount.close()
                if mount.p.returncode != 0 or os.path.ismount(mount.path):
                    cleanup.append(f'client rc={mount.p.returncode} mounted={os.path.ismount(mount.path)}')
            except Exception as e:
                cleanup.append(f'unmount: {e!r}')
        daemons = dict(c.procs)
        try:
            c.stop_all()
        except Exception as e:
            cleanup.append(f'daemon stop: {e!r}')
        for i, proc in daemons.items():
            if proc.returncode != 0:
                cleanup.append(f'node {i} rc={proc.returncode}')
        if cleanup:
            result['cleanup_errors'] = cleanup
            result['outcome'] = 'FAIL'
        save()
    print(json.dumps(result), flush=True)
    return 0 if result['outcome'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
