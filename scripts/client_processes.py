#!/usr/bin/env python3
"""Discover Linux FUSE clients independently of mount visibility.
Retirement requires successful control-channel drain by client.sh first.
"""
import os
from pathlib import Path
import signal
import sys
import time


def clients(proc=Path('/proc')):
    result = []
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if (entry/'comm').read_text().strip() != 'efs-fuse':
                continue
            fields = (entry/'stat').read_text().rsplit(')', 1)[1].split()
            if fields[0] in ('Z', 'X'):
                continue
            args = (entry/'cmdline').read_bytes().split(b'\0')
            if len(args) > 1 and args[1] in (b'--stop', b'--resume', b'--version', b'--help'):
                continue
            if len(args) < 4 or args[1].startswith(b'--'):
                # Exit can clear cmdline after the first stat read. Confirm
                # death or PID reuse before treating this as a live ambiguity.
                current = (entry/'stat').read_text().rsplit(')', 1)[1].split()
                if current[0] in ('Z', 'X') or current[19] != fields[19]:
                    continue
                raise RuntimeError(f'cannot identify mount for efs-fuse PID {entry.name}')
            mount = os.fsdecode(args[3])
            if not mount.startswith('/'):
                mount = os.readlink(entry/'cwd') + '/' + os.fsdecode(args[3])
            # starttime distinguishes PID reuse; never resolve the FUSE path.
            stamp = fields[19]
            if '\n' in mount or '\r' in mount:
                raise RuntimeError(f'unsupported mount pathname for PID {entry.name}')
            result.append((int(entry.name), os.path.normpath(mount), stamp))
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError as exc:
            raise RuntimeError(f'client discovery requires root: {entry}') from exc
    return result


def retire(mount):
    mount = os.path.abspath(mount)
    initial = [row for row in clients() if row[1] == mount]
    # client.sh has quiesced and drained this pathname. Multiple owners are
    # ambiguous: never assume one successful DRAIN authorizes killing all.
    if len(initial) > 1:
        raise RuntimeError(f'multiple clients for {mount}; refusing retirement')
    for pid, path, stamp in initial:
        if (pid, path, stamp) not in clients():
            continue
        # Only the process owning this control endpoint was drained.
        value = 14695981039346656037
        for byte in os.fsencode(path):
            value = ((value ^ byte) * 1099511628211) & ((1 << 64)-1)
        endpoint = f'/tmp/efs-control-{value:016x}.sock'
        inodes = {line.split()[6] for line in Path('/proc/net/unix').read_text().splitlines()[1:]
                  if len(line.split()) >= 8 and line.split()[7] == endpoint}
        links = {os.readlink(fd) for fd in Path(f'/proc/{pid}/fd').iterdir()}
        if not any(f'socket:[{inode}]' in links for inode in inodes):
            raise RuntimeError(f'PID {pid} does not own drained endpoint; retained')
        print(f'Retiring drained efs-fuse PID {pid} for {path}', flush=True)
        if not hasattr(os, 'pidfd_open') or not hasattr(signal, 'pidfd_send_signal'):
            raise RuntimeError('safe retirement requires Linux pidfd support')
        fd = os.pidfd_open(pid)
        try:
            if (pid, path, stamp) in clients():
                signal.pidfd_send_signal(fd, signal.SIGTERM)
        finally:
            os.close(fd)
    deadline = time.monotonic() + 10
    while any(row[1] == mount for row in clients()):
        if time.monotonic() >= deadline:
            raise RuntimeError(f'client remains alive for {mount}; no forced kill attempted')
        time.sleep(.1)


def main():
    command = sys.argv[1]
    if command == 'list':
        mounts = {row[1] for row in clients()}
        # mountinfo covers clients whose daemon has died as well.
        for line in Path('/proc/self/mountinfo').read_text().splitlines():
            left, right = line.split(' - ', 1)
            if right.split()[0] == 'fuse.efs-fuse':
                raw = left.split()[4]
                for code, char in [('040', ' '), ('011', '\t'), ('012', '\n'), ('134', '\\')]:
                    raw = raw.replace('\\'+code, char)
                mounts.add(raw)
        for mount in sorted(mounts):
            if '\n' in mount or '\r' in mount:
                raise RuntimeError('unsupported mount pathname; discovery refused')
            print(mount)
    elif command == 'has':
        return 0 if any(row[1] == os.path.abspath(sys.argv[2]) for row in clients()) else 1
    elif command == 'retire':
        retire(sys.argv[2])
    else:
        raise RuntimeError('usage: client_processes.py list|has|retire [mount]')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, OSError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(2)
