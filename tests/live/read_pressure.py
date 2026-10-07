#!/usr/bin/env python3
"""W60 live gate. Prepare a NEW fixture, then run against a 32 MiB client.

python3 tests/live/read_pressure.py prepare /mount --fixture w60-unique
python3 tests/live/read_pressure.py run /mount --fixture w60-unique
Only prepare writes data; run checks 2000 tiny reads under sequential pressure.
Fixtures are retained for A/B reruns; remove the named fixture after testing.
"""
import argparse
import json
import os
from pathlib import Path
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('prepare', 'run'))
    parser.add_argument('mount', type=Path)
    parser.add_argument('--fixture', required=True)
    args = parser.parse_args()
    if not args.fixture or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in args.fixture):
        parser.error('fixture must be one directory name using letters, numbers, - or _')
    root = args.mount / args.fixture
    block = bytes(range(256)) * 4096
    marker = {'efs_w60_fixture': 1, 'files': 2000, 'size_mib': 512}
    if args.mode == 'prepare':
        root.mkdir()  # Never overwrite an existing fixture/user directory.
        with (root / 'sequential').open('wb', buffering=0) as f:
            for _ in range(marker['size_mib']):
                assert f.write(block) == len(block)
            os.fsync(f.fileno())
        for i in range(marker['files']):
            with (root / f'tiny-{i}').open('wb') as f:
                f.write(b'abc')
        (root / 'fixture.json').write_text(json.dumps(marker))
        os.sync()
        print('fixtures ready', flush=True)
        return
    assert json.loads((root / 'fixture.json').read_text()) == marker
    stop, ready = threading.Event(), threading.Event()
    stats = {'sequential_bytes': 0, 'sequential_errors': []}

    def reader():
        try:
            while not stop.is_set():
                with (root / 'sequential').open('rb', buffering=0) as f:
                    while not stop.is_set():
                        data = f.read(131072)
                        if not data:
                            break
                        if data != block[:len(data)]:
                            raise AssertionError('sequential data mismatch')
                        stats['sequential_bytes'] += len(data)
                        if stats['sequential_bytes'] >= 16 * 1024 * 1024:
                            ready.set()
        except Exception as exc:
            stats['sequential_errors'].append(str(exc))
            ready.set()

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    failures = []
    started = time.monotonic()
    try:
        assert ready.wait(30), 'sequential reader did not start'
        time.sleep(.2)
        for i in range(marker['files']):
            if time.monotonic() - started > 120:
                raise TimeoutError('mixed-read gate exceeded 120 seconds')
            try:
                with (root / f'tiny-{i}').open('rb', buffering=0) as f:
                    assert f.read(4) == b'abc', 'tiny data mismatch'
            except Exception as exc:
                failures.append((i, str(exc)))
    finally:
        stop.set()
        thread.join(30)
    result = dict(stats, failures=len(failures), first_failures=failures[:5],
                  elapsed=time.monotonic() - started, reader_stopped=not thread.is_alive())
    print(json.dumps(result), flush=True)
    assert not failures and not stats['sequential_errors'] and not thread.is_alive()


if __name__ == '__main__':
    main()
