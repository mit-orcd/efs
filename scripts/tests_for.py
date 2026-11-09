#!/usr/bin/env python3
"""tests_for.py SUBSYSTEM [--run]: the test catalogue's commands for a subsystem.

tests/budgets.tsv classifies every `make test` and `make test-long` command by
subsystem: a source directory plus an optional component, for example
server/raft_host or server/raft_host.read_index. A row that exercises more
than one component carries every tag, comma-separated (client/write,
client/efs_fuse.release); a command is listed once however many of its tags
match. SUBSYSTEM selects the rows with a tag equal to it or starting with it
followed by a dot, so server/raft_host includes server/raft_host.read_index.
A component with no row of its own gets its parent's rows with a note on
stderr, but only when the component exists: src/<dir>/<component>.c or .h,
or a src/<dir>/<component>/ directory (meta/meta_apply.publish). A name that
matches neither a tag nor a source file is refused (exit 2) so that a
misspelling is not answered with the parent's tests. The commands print one
per line in catalogue order; --run executes them from the repository root, in
order, and stops with the first failing command's exit status. Build first
(`make all`): the commands expect their binaries.
"""
import subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]


def catalogue(path=None):
    """(command, target, tags) per row of the catalogue."""
    for line in (path or root / 'tests/budgets.tsv').read_text().splitlines():
        f = line.split('\t')
        if len(f) >= 7 and not line.startswith('#') and f[0] != 'command':
            yield f[0], f[4], [t for t in f[5].split(',') if t]


def select(rows, want):
    """The rows with a tag under `want`, each command once, in catalogue order."""
    out, seen = [], set()
    for cmd, target, tags in rows:
        if cmd not in seen and any(t == want or t.startswith(want + '.') for t in tags):
            seen.add(cmd)
            out.append((cmd, target))
    return out


def component_exists(sub, src=None):
    """src/<dir>/<component>.c, .h or /: the component is real, if untested."""
    src = src or root / 'src'
    path = src / sub.replace('.', '/')
    return any(p.exists() for p in (path.with_suffix('.c'), path.with_suffix('.h'), path))


def resolve(rows, want, src=None):
    """(rows, subsystem answered) for `want`; LookupError when nothing fits:
    'unknown component' for a misspelling (exit 2), 'no catalogue row' else."""
    selected = select(rows, want)
    sub = want
    while not selected and '.' in sub:
        if not component_exists(sub, src):
            raise LookupError('tests_for: unknown component %s: no catalogue row and no '
                              'src/%s.{c,h} or directory (misspelled? see tests/budgets.tsv)'
                              % (sub, sub.replace('.', '/')))
        sub = sub.rsplit('.', 1)[0]  # a real component with no row of its own
        selected = select(rows, sub)
    if not selected:
        raise LookupError('tests_for: no catalogue row for subsystem %s (see tests/budgets.tsv)' % want)
    return selected, sub


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
    try:
        rows, answered = resolve(list(catalogue()), args[0])
    except LookupError as e:
        print(e, file=sys.stderr)
        sys.exit(2 if 'unknown component' in str(e) else 1)
    if answered != args[0]:
        print('tests_for: no row of its own for %s; the rows of %s follow' % (args[0], answered), file=sys.stderr)
    for cmd, target in rows:
        if '--run' not in argv:
            print(cmd)
            continue
        print('+', cmd, flush=True)
        rc = subprocess.run(['bash', '-c', full_command(cmd, target)], cwd=root).returncode
        if rc:
            sys.exit(rc)


if __name__ == '__main__':
    main(sys.argv[1:])
