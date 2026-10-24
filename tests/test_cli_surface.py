#!/usr/bin/env python3
"""F2-F5: the efs-mgmt help and dispatcher name the same commands.

Every command efs-mgmt dispatches is in the overview help and answers
`efs-mgmt COMMAND --help` with its usage (exit 0); every overview entry and
every argument spec dispatches. Removed commands (add-node, add-storage)
are refused by name, with or without --help, and name the supported way.
efs-fuse refuses the removed --bench flags instead of passing them to FUSE.
Runs the built binaries; no daemon or network."""
from pathlib import Path
import re, subprocess, sys

root = Path(__file__).resolve().parents[1]
mgmt = str(root / 'efs-mgmt')
fuse = str(root / 'efs-fuse')
src = (root / 'src/mgmt/efs_mgmt.c').read_text()
helph = (root / 'include/efs/cli_help.h').read_text()
REMOVED = {'add-node': 'efsd --join', 'add-storage': 'fixed when its store is created'}


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, timeout=10)


# Dispatched names in main(), split by the expiry build switch.
main = src[src.index('int main(int argc, char **argv)'):]
dispatch, expiry_only, gated = set(), set(), False
for line in main.splitlines():
    if line.startswith('#if EFS_BUILD_EXPIRY'):
        gated = True
    elif line.startswith('#endif'):
        gated = False
    for name in re.findall(r'strcmp\(cmd,\s*"([^"]+)"\)\s*==\s*0', line):
        (expiry_only if gated else dispatch).add(name)
assert 'status' in dispatch and 'raft-resync' in dispatch, dispatch

overview = run(mgmt, '--help')
assert overview.returncode == 0, overview
text = overview.stdout
block = text[text.index('\nCommands:\n') + 11:]
block = block[:block.index('\n\n')]
listed = set()
for line in block.splitlines():
    names = re.split(r'\s{2,}', line.strip())[0]
    listed.update(n.strip() for n in names.split(','))
expiry_build = bool(listed & expiry_only)
if expiry_build:
    dispatch |= expiry_only

specs = set(re.findall(r'\{"([a-z-]+)","', helph[helph.index('efs_mgmt_help'):]))
if not expiry_build:
    specs -= expiry_only

missing_help = dispatch - listed
assert not missing_help, f'dispatched but not in the overview help: {sorted(missing_help)}'
stale = listed - dispatch
assert not stale, f'in the overview help but not dispatched: {sorted(stale)}'
assert dispatch == specs, f'argument specs differ: missing {sorted(dispatch - specs)}, extra {sorted(specs - dispatch)}'

for name in sorted(dispatch):
    r = run(mgmt, name, '--help')
    assert r.returncode == 0 and f'Usage:\n  efs-mgmt {name} ' in r.stdout, (name, r)

for name, hint in REMOVED.items():
    assert name not in listed and name not in specs and name not in dispatch, name
    for args in ((name,), (name, '127.0.0.1:1', '/x'), (name, '--help')):
        r = run(mgmt, *args)
        assert r.returncode == 2 and 'was removed' in r.stderr and hint in r.stderr, (args, r)

if Path(fuse).exists():
    for flag in ('--bench', '--bench-time', '--bench-mib'):
        r = run(fuse, '127.0.0.1:1', '/nonexistent-mount', flag, '1')
        assert r.returncode == 2 and f'{flag} was removed' in r.stderr, (flag, r)
    r = run(fuse, '--help')
    assert r.returncode == 0 and '--bench' not in r.stdout, r

print(f'cli surface: {len(dispatch)} efs-mgmt commands dispatched, listed and with --help; '
      f'removed add-node/add-storage and efs-fuse --bench refused PASS')
