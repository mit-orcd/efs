#!/usr/bin/env python3
"""Real, chunk-aligned payload and namespace oracle; synthetic tests are separate.

Fresh private root only. Default is 1,638,400 files * 256 KiB = 400 GiB.
Use the planned separate-port, separately provisioned fixture, never production
quotas. Every file is fsynced/closed before acknowledgement; verify independently
derives every payload and namespace entry through a fresh cold client.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('phase', choices=('write', 'resume', 'verify'))
p.add_argument('mount', type=Path)
p.add_argument('fixture')
p.add_argument('results', type=Path)
p.add_argument('--files', type=int, default=1638400)
p.add_argument('--bytes', type=int, default=256 << 10)
p.add_argument('--workers', type=int, default=8)
p.add_argument('--resume-from', type=Path,
               help='immutable failed-write checkpoint directory; rehash acknowledged bytes before continuing')
a = p.parse_args()
assert a.fixture and '/' not in a.fixture and a.fixture not in ('.', '..')
assert 1 <= a.files <= 1638400 and 1 <= a.workers <= 8
assert 0 < a.bytes <= 256 << 10 and a.bytes % 32 == 0
a.mount = a.mount.resolve()
a.results = a.results.resolve()
assert not a.results.is_relative_to(a.mount), 'keep independent evidence outside EFS'
a.results.mkdir(parents=True, exist_ok=True)
root = a.mount / a.fixture
stop = threading.Event()
completed = [0] * a.workers
started = time.monotonic()
acknowledged = [0] * a.workers
assert (a.phase == 'resume') == (a.resume_from is not None)
if a.resume_from is not None:
    assert a.resume_from.resolve() != a.results, 'preserve original failed-run evidence'
    for number in range(a.workers):
        record = json.loads((a.resume_from / ('write-worker-%02d.json' % number)).read_text())
        assert record['worker'] == number and record['phase'] == 'write'
        assert record['expected_files'] == a.files and record['bytes_per_file'] == a.bytes
        acknowledged[number] = record['completed']
        assert 0 <= acknowledged[number] <= a.files * (number + 1) // a.workers - a.files * number // a.workers

def mounted():
    # Never fork this process while another worker has a direct read in flight.
    # The open directory's mount ID also identifies stacked mounts exactly.
    fd = os.open(a.mount, os.O_PATH | os.O_DIRECTORY)
    try:
        info = Path('/proc/self/fdinfo/%d' % fd).read_text().splitlines()
        mount_id = next(line.split()[1] for line in info if line.startswith('mnt_id:'))
        filesystem = next((line.split(' - ', 1)[1].split()[0]
                           for line in Path('/proc/self/mountinfo').read_text().splitlines()
                           if line.split()[0] == mount_id), None)
        assert filesystem == 'fuse.efs-fuse', 'refuse local/NFS fallback'
    finally:
        os.close(fd)


def payload(index):
    # Nonzero actual writes. XFS/files does not deduplicate this bounded buffer.
    return hashlib.sha256(b'W92-real-payload:' + index.to_bytes(8, 'big')).digest() * (a.bytes // 32)

def span(worker):
    return a.files * worker // a.workers, a.files * (worker + 1) // a.workers

def path(worker, index):
    begin, _ = span(worker)
    return root / ('worker-%02d' % worker) / ('dir-%06d' % ((index - begin) // 1024)) / ('file-%09d' % index)

def progress(worker):
    record = dict(worker=worker, completed=completed[worker], phase=a.phase,
                  expected_files=a.files, bytes_per_file=a.bytes,
                  elapsed_seconds=time.monotonic() - started)
    temporary = a.results / ('%s-worker-%02d.tmp' % (a.phase, worker))
    final = temporary.with_suffix('.json')
    with open(temporary, 'w') as stream:
        json.dump(record, stream)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, final)

def worker(number):
    begin, end = span(number)
    try:
        if a.phase == 'write':
            (root / ('worker-%02d' % number)).mkdir()
        for index in range(begin, end):
            if stop.is_set():
                return
            if (index - begin) % 128 == 0:
                mounted()
                progress(number)
            target = path(number, index)
            expected = payload(index)
            if a.phase in ('write', 'resume') and index - begin >= acknowledged[number]:
                if (index - begin) % 1024 == 0:
                    target.parent.mkdir(exist_ok=a.phase == 'resume')
                # Only an unacknowledged tail may be rewritten after interruption.
                # All acknowledged payloads take the independent read/hash path.
                mode = 'xb'
                if a.phase == 'resume' and target.exists():
                    print(json.dumps(dict(recover_unacknowledged=str(target),
                                          previous_bytes=target.stat().st_size)), flush=True)
                    mode = 'wb'
                with open(target, mode, buffering=0) as stream:
                    offset = 0
                    while offset < len(expected):
                        written = stream.write(memoryview(expected)[offset:])
                        assert written and written > 0
                        offset += written
                    os.fsync(stream.fileno())
            else:
                digest = hashlib.sha256()
                length = 0
                chunks = []
                with open(target, 'rb', buffering=0) as stream:
                    while True:
                        chunk = stream.read(64 << 10)
                        if not chunk:
                            break
                        digest.update(chunk)
                        chunks.append(chunk)
                        length += len(chunk)
                if length != a.bytes or digest.digest() != hashlib.sha256(expected).digest():
                    # Preserve the actual failing read, never a successful reread.
                    name = 'mismatch-worker-%02d-file-%09d' % (number, index)
                    (a.results / (name + '.bin')).write_bytes(b''.join(chunks))
                    (a.results / (name + '.json')).write_text(json.dumps(dict(
                        path=str(target), index=index, length=length,
                        expected_length=a.bytes, actual_sha256=digest.hexdigest(),
                        expected_sha256=hashlib.sha256(expected).hexdigest(),
                        chunk_lengths=[len(chunk) for chunk in chunks],
                        chunk_sha256=[hashlib.sha256(chunk).hexdigest() for chunk in chunks]
                    ), sort_keys=True) + '\n')
                    raise AssertionError(target)
            completed[number] += 1
        if a.phase == 'verify':
            folder = root / ('worker-%02d' % number)
            assert set(os.listdir(folder)) == {'dir-%06d' % d for d in range((end - begin + 1023) // 1024)}
            for offset in range(0, end - begin, 1024):
                target = path(number, begin + offset).parent
                assert set(os.listdir(target)) == {'file-%09d' % i for i in range(begin + offset, min(end, begin + offset + 1024))}, target
    except BaseException:
        stop.set()
        raise
    finally:
        progress(number)

mounted()
if a.phase == 'write':
    root.mkdir()  # refuse a reused partial fixture
else:
    assert root.is_dir()
with concurrent.futures.ThreadPoolExecutor(max_workers=a.workers) as pool:
    tasks = [pool.submit(worker, number) for number in range(a.workers)]
    for task in tasks:
        task.result()
assert sum(completed) == a.files
assert set(os.listdir(root)) == {'worker-%02d' % number for number in range(a.workers)}
expected_dirs = a.workers + sum((span(w)[1] - span(w)[0] + 1023) // 1024 for w in range(a.workers))
deadline = time.monotonic() + 600
while True:
    reply = subprocess.run([sys.executable, str(root / '.efs/stats'), '--json'],
                           capture_output=True, timeout=120)
    assert not reply.returncode, reply.stderr
    stats = json.loads(reply.stdout)
    if stats['stats']['tree_files'] == a.files and stats['stats']['tree_bytes'] == a.files * a.bytes:
        assert stats['stats']['tree_dirs'] == expected_dirs, stats
        assert stats['snapshot']['refresh_error'] == 0, stats
        break
    assert time.monotonic() < deadline, stats
    time.sleep(1)
exported = None
if a.phase == 'verify':
    output = a.results / 'query-paths.nul'
    command = subprocess.run([sys.executable, str(root / '.efs/find'),
                              'file-', '--match', 'prefix', '-0', '--file', str(output)],
                             capture_output=True, timeout=86400)
    assert not command.returncode, command.stderr
    seen = bytearray((a.files + 7) // 8)
    exported = 0
    prefix = os.fsencode(root) + b'/'
    pending = b''
    with open(output, 'rb') as stream:
        while True:
            chunk = stream.read(64 << 10)
            if not chunk:
                break
            parts = (pending + chunk).split(b'\0')
            pending = parts.pop()
            assert len(pending) <= 4096, 'unbounded query path'
            for entry in parts:
                assert entry.startswith(prefix), entry
                relative = entry[len(prefix):].decode('ascii').split('/')
                assert len(relative) == 3, entry
                assert relative[0].startswith('worker-') and relative[2].startswith('file-'), entry
                number = int(relative[0][7:])
                index = int(relative[2][5:])
                assert 0 <= number < a.workers and span(number)[0] <= index < span(number)[1], entry
                assert os.fsencode(path(number, index)) == entry, entry
                bit = 1 << (index % 8)
                assert not seen[index // 8] & bit, 'duplicate query path'
                seen[index // 8] |= bit
                exported += 1
    assert not pending and exported == a.files, (len(pending), exported)
record = dict(result='PASS', phase=a.phase, fixture=str(root), files=a.files,
              payload_bytes=a.files * a.bytes, directories=expected_dirs,
              files_fsynced=a.files if a.phase == 'write' else
                            a.files - sum(acknowledged) if a.phase == 'resume' else None,
              payloads_verified=a.files if a.phase == 'verify' else
                                sum(acknowledged) if a.phase == 'resume' else None,
              query_paths_verified=exported,
              seconds=time.monotonic() - started, snapshot=stats['snapshot'])
(a.results / (a.phase + '-complete.json')).write_text(json.dumps(record, sort_keys=True) + '\n')
print(json.dumps(record, sort_keys=True), flush=True)
