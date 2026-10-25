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
    parser.add_argument('--backlog-files',type=int,default=0,help='1000..100000 files per expiry backlog phase')
    parser.add_argument('--soak-seconds', type=int, default=0, help='optional expiry churn soak, 60..7200 seconds')
    parser.add_argument('--clock-fault', action='store_true',
                        help='also test clock-health pause/reset; requires EFS_FAULTS=1 binaries')
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65532:
        parser.error('--port must leave room for four node ports')
    if args.soak_seconds and not 60 <= args.soak_seconds <= 7200:
        parser.error('--soak-seconds must be 60..7200, or zero')
    if args.backlog_files and not 1000 <= args.backlog_files <= 100000:
        parser.error('--backlog-files must be 1000..100000, or zero')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.mount.resolve() == args.peer_mount.resolve():
        parser.error('primary and peer mount paths must differ')
    source = Path(__file__).resolve().parents[2]
    cmd = [sys.executable, str(source / 'tests/live/gc_reclamation.py'),
           '--expiry', '--transport', 'tcp',
           '--disable-efs-commands', '--mount', str(args.mount.resolve()),
           '--peer-mount', str(args.peer_mount.resolve()), '--port', str(args.port),
           '--posix', '--posix-jobs', str(args.jobs)]
    roots = args.root * 2 if len(args.root) == 1 else args.root
    if len(roots) != 2:
        parser.error("provide one storage parent or two --root arguments")
    for root in roots:
        cmd.extend(('--root', str(root.resolve())))
    if args.soak_seconds:
        cmd.extend(("--expiry-soak-seconds", str(args.soak_seconds)))
    if args.backlog_files:
        cmd.extend(('--expiry-backlog-files',str(args.backlog_files)))
    if args.clock_fault:
        cmd.append('--expiry-clock-fault')
    env = dict(os.environ, EFS_COMMANDS='0',
               EFS_COMMANDS_SERVER='0', EFS_TRANSPORT='tcp')
    return cmd, env


def main(argv=None):
    cmd, env = command(sys.argv[1:] if argv is None else argv)
    for i, arg in enumerate(cmd[:-1]):
        if arg == '--root':
            Path(cmd[i + 1]).mkdir(parents=True, exist_ok=True)
    print('POSIX expiry: isolated lifecycle, recovery, physical GC, core and peer gates', flush=True)
    return subprocess.call(cmd, env=env)


if __name__ == '__main__':
    sys.exit(main())
