#!/usr/bin/env python3
"""tests_for.py SUBSYSTEM [--run]: the test catalogue's commands for a subsystem.

tests/budgets.tsv classifies every `make test` and `make test-long` command by
subsystem (a source directory plus an optional component, for example
server/raft_host or server/raft_host.read_index). SUBSYSTEM selects the rows
whose subsystem equals it or starts with it followed by a dot, so
server/raft_host includes server/raft_host.read_index; a component with no
row of its own (server/raft_host.publish) gets its parent's rows, with a note
on stderr. The commands print one
per line in catalogue order; --run executes them from the repository root, in
order, and stops with the first failing command's exit status. Build first
(`make all`): the commands expect their binaries.
"""
import subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]


def catalogue():
    for line in (root / 'tests/budgets.tsv').read_text().splitlines():
        f = line.split('\t')
        if len(f) >= 7 and not line.startswith('#') and f[0] != 'command':
            yield f[0], f[4], f[5]


def full_command(cmd, target):
    """The catalogue keys a multi-line recipe command by its first line."""
    if not cmd.endswith('\\'):
        return cmd
    out = subprocess.run(['make', '-n', target], cwd=root, capture_output=True, text=True, check=True).stdout.splitlines()
    i = out.index(cmd)
    j = i
    while out[j].endswith('\\'):
        j += 1
    return '\n'.join(out[i:j + 1])


def main(argv):
    args = [a for a in argv if a != '--run']
    if len(args) != 1 or args[0].startswith('-'):
        sys.exit('usage: tests_for.py SUBSYSTEM [--run]')
    want = args[0]
    def select(sub):
        return [(c, t) for c, t, s in catalogue() if s == sub or s.startswith(sub + '.')]
    rows = select(want)
    while not rows and '.' in want:  # a component with no row of its own: its parent's rows
        want = want.rsplit('.', 1)[0]
        rows = select(want)
    if not rows:
        sys.exit('tests_for: no catalogue row for subsystem %s (see tests/budgets.tsv)' % want)
    if want != args[0]:
        print('tests_for: no row of its own for %s; the rows of %s follow' % (args[0], want), file=sys.stderr)
    for cmd, target in rows:
        if '--run' not in argv:
            print(cmd)
            continue
        print('+', cmd, flush=True)
        rc = subprocess.run(['bash', '-c', full_command(cmd, target)], cwd=root).returncode
        if rc:
            sys.exit(rc)


main(sys.argv[1:])
