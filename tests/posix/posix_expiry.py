#!/usr/bin/env python3
"""Expiry lifecycle suite using an owned four-node TCP/direct-I/O fixture.

Invoked by posix_suite.py MOUNT --suite expiry and by the d5-expiry gate of
scripts/qualify.py; never attaches to a user cluster. Runs
tests/live/gc_reclamation.py --expiry --expiry-only: the cluster expiry
policy (D43) off by default, then on, off and on at runtime; the expiry
lifecycle, folder-policy coverage, restarts, member and leader loss and
physical reclamation; then (unless --no-posix) the POSIX core suite on the
cluster with expiry on. .efs is off by the cluster commands-policy.

`posix_expiry.py --private` (make test-long) owns everything: mount paths
and stores in a new directory under TMPDIR (deleted after a PASS, kept for
diagnosis after a failure), four free loopback ports below the ephemeral
range, no POSIX core run.
It prints NOTRUN and exits 0 without /dev/fuse, fusermount3 or efs-fuse.
"""
import argparse
import os
from pathlib import Path
import random
import shutil
import socket
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[2]


def free_base():
    """The first of four consecutive free loopback ports, chosen below the
    kernel's ephemeral range: concurrent tests that take ports with bind(0)
    can never be handed one of them while this fixture starts (gate pool).
    Outside the reserved 17432-17435 and 27432-27433."""
    low = 32768
    try:
        low = int(Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().split()[0])
    except (OSError, ValueError, IndexError):
        pass
    rng = random.Random()
    for _ in range(200):
        base = rng.randrange(20000, max(20001, low - 4))
        if any(p in (17432, 17433, 17434, 17435, 27432, 27433) for p in range(base, base + 4)):
            continue
        held = []
        try:
            for port in range(base, base + 4):
                s = socket.socket()
                held.append(s)
                s.bind(('127.0.0.1', port))
            return base
        except OSError:
            continue
        finally:
            for s in held:
                s.close()
    raise SystemExit('no four consecutive free loopback ports')


def command(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mount', type=Path, nargs='?', help='empty, unmounted, FUSE-policy-approved mount path')
    parser.add_argument('--suite', choices=('expiry',))
    parser.add_argument('--private', action='store_true',
                        help='own mount paths, stores and ports under TMPDIR; implies --no-posix (make test-long)')
    parser.add_argument('--root', type=Path, action='append',
                        help='private storage parent (repeat for node placement); existing user stores are not used')
    parser.add_argument('--peer-mount', type=Path,
                        help='second empty, unmounted, FUSE-policy-approved mount path')
    parser.add_argument('--port', type=int, default=24410, help='first of four free node ports (default: 24410)')
    parser.add_argument('--jobs', type=int, default=4, help='core POSIX concurrency (default: 4)')
    parser.add_argument('--timeout-s', type=int, default=30,
                        help='per-case POSIX timeout (default: 30; one-disk hosts need the topology value, W159)')
    parser.add_argument('--no-posix', action='store_true', help='expiry phases only, no POSIX core run')
    parser.add_argument('--backlog-files',type=int,default=0,help='1000..100000 files per expiry backlog phase')
    parser.add_argument('--soak-seconds', type=int, default=0, help='optional expiry churn soak, 60..7200 seconds')
    parser.add_argument('--clock-fault', action='store_true',
                        help='also test clock-health pause/reset; requires EFS_FAULTS=1 binaries')
    args = parser.parse_args(argv)
    private = None
    if args.private:
        if args.mount or args.root or args.peer_mount:
            parser.error('--private chooses its own mount paths and stores')
        private = Path(tempfile.mkdtemp(prefix='efs-posix-expiry-', dir=os.environ.get('TMPDIR')))
        for name in ('mnt', 'peer', 'r1', 'r2'):
            (private / name).mkdir()
        args.mount, args.peer_mount = private / 'mnt', private / 'peer'
        args.root = [private / 'r1', private / 'r2']
        args.port = free_base()
        args.no_posix = True
    elif args.suite != 'expiry' or not args.mount or not args.root or not args.peer_mount:
        parser.error('MOUNT, --suite expiry, --root and --peer-mount are required (or --private)')
    if not 1 <= args.port <= 65532:
        parser.error('--port must leave room for four node ports')
    if args.soak_seconds and not 60 <= args.soak_seconds <= 7200:
        parser.error('--soak-seconds must be 60..7200, or zero')
    if args.backlog_files and not 1000 <= args.backlog_files <= 100000:
        parser.error('--backlog-files must be 1000..100000, or zero')
    if args.jobs < 1 or args.timeout_s < 1:
        parser.error('--jobs and --timeout-s must be positive')
    if args.mount.resolve() == args.peer_mount.resolve():
        parser.error('primary and peer mount paths must differ')
    cmd = [sys.executable, str(SOURCE / 'tests/live/gc_reclamation.py'),
           '--expiry', '--transport', 'tcp',
           '--expiry-only', '--disable-efs-commands', '--mount', str(args.mount.resolve()),
           '--peer-mount', str(args.peer_mount.resolve()), '--port', str(args.port)]
    if not args.no_posix:
        cmd += ['--posix', '--posix-jobs', str(args.jobs), '--posix-timeout-s', str(args.timeout_s)]
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
    # D42/D43: no environment switch for .efs or expiry (efsd refuses them).
    env = {k: v for k, v in os.environ.items()
           if k not in ('EFS_COMMANDS', 'EFS_COMMANDS_SERVER', 'EFS_EXPIRY')}
    env['EFS_TRANSPORT'] = 'tcp'
    return cmd, env, private


def fuse_ok():
    return (SOURCE / 'efs-fuse').exists() and os.access('/dev/fuse', os.R_OK | os.W_OK) and \
        bool(shutil.which('fusermount3'))


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if '--private' in argv and not fuse_ok():
        print('POSIX expiry: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)', flush=True)
        return 0
    cmd, env, private = command(argv)
    for i, arg in enumerate(cmd[:-1]):
        if arg == '--root':
            Path(cmd[i + 1]).mkdir(parents=True, exist_ok=True)
    print('POSIX expiry: isolated lifecycle, recovery, physical GC%s' %
          ('' if '--posix' in cmd else ' (no POSIX core run)'), flush=True)
    rc = subprocess.call(cmd, env=env)
    if private:
        if rc == 0:
            shutil.rmtree(private, ignore_errors=True)
        else:
            print('kept for diagnosis:', private, flush=True)
    return rc


if __name__ == '__main__':
    sys.exit(main())
