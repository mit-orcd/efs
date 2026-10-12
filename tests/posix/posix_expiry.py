#!/usr/bin/env python3
"""Expiry lifecycle suite using an owned four-node TCP/direct-I/O fixture.

Invoked by posix_suite.py MOUNT --suite expiry; never attaches to a user cluster.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def command(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mount', type=Path, help='empty, unmounted, FUSE-policy-approved mount path')
    parser.add_argument('--suite', choices=('expiry',), required=True)
    parser.add_argument('--root', type=Path, action='append', required=True,
                        help='private storage parent (repeat for node placement); existing user stores are not used')
    parser.add_argument('--peer-mount', type=Path, required=True,
                        help='second empty, unmounted, FUSE-policy-approved mount path')
    parser.add_argument('--port', type=int, default=24410, help='first of four free node ports (default: 24410)')
    parser.add_argument('--jobs', type=int, default=4, help='core POSIX concurrency (default: 4)')
    parser.add_argument('--clock-fault', action='store_true',
                        help='also test clock-health pause/reset; requires EFS_FAULTS=1 binaries')
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65532:
        parser.error('--port must leave room for four node ports')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.mount.resolve() == args.peer_mount.resolve():
        parser.error('primary and peer mount paths must differ')
    source = Path(__file__).resolve().parents[2]
    cmd = [sys.executable, str(source / 'tests/live/gc_reclamation.py'),
           '--expiry', '--mode', 'direct', '--transport', 'tcp',
           '--disable-efs-commands', '--mount', str(args.mount.resolve()),
           '--peer-mount', str(args.peer_mount.resolve()), '--port', str(args.port),
           '--posix', '--posix-jobs', str(args.jobs)]
    for root in args.root:
        cmd.extend(('--root', str(root.resolve())))
    if args.clock_fault:
        cmd.append('--expiry-clock-fault')
    env = dict(os.environ, EFS_PACKED_WRITES='0', EFS_COMMANDS='0',
               EFS_COMMANDS_SERVER='0', EFS_TRANSPORT='tcp')
    return cmd, env


def main(argv=None):
    cmd, env = command(sys.argv[1:] if argv is None else argv)
    print('POSIX expiry: isolated lifecycle, recovery, physical GC, core and peer gates', flush=True)
    return subprocess.call(cmd, env=env)


if __name__ == '__main__':
    sys.exit(main())
