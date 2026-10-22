#!/usr/bin/env python3
"""The enabled-command gate's cleanup contract matches the fixtures it runs.

W120: `tests/live/gc_reclamation.py --efs-commands-only` runs
`efs_commands.py` and `efs_command_options.py`, then removes only the trees
that appeared under the mount during that gate, and refuses to continue if
any of them has a name it does not own. The options script moved to a
unique `efs-options-gates-<time_ns>` name while the harness still accepted
the fixed `efs-options-gates`, so a passing gate failed at the ownership
assertion. This keeps the three files in step without a cluster.
"""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
harness = (root / 'tests/live/gc_reclamation.py').read_text()
commands = (root / 'tests/live/efs_commands.py').read_text()
options = (root / 'tests/live/efs_command_options.py').read_text()

# The harness: one ownership pattern, applied to the trees created during the gate only.
m = re.search(r"assert created and all\(re\.fullmatch\(r'([^']+)',n\) for n in created\),created", harness)
assert m, 'gc_reclamation.py: ownership assertion on the created trees not found'
pattern = m.group(1)
assert "created={p.name for p in mount.iterdir()}-before" in harness, 'created trees are not the difference against the pre-gate listing'
assert "for name in created:shutil.rmtree(mount/name)" in harness, 'cleanup must remove only the trees this gate created'

# The fixtures: their names and suffix sources.
c = re.search(r"name = '(efs-command-gates-)%d' % os\.getpid\(\)", commands)
o = re.search(r"root = mount / \('(efs-options-gates-)'\+str\(time\.time_ns\(\)\)\)", options)
assert c and o, 'fixture name expressions changed; update this contract and the harness together'
for sample in (c.group(1) + '4242', o.group(1) + str(1_761_000_000_000_000_000)):
    assert re.fullmatch(pattern, sample), 'harness pattern %r rejects the fixture name %r' % (pattern, sample)
# Not owned: the old fixed name, a non-numeric suffix, nested paths, foreign trees.
for foreign in ('efs-options-gates', 'efs-command-gates', 'efs-options-gates-abc', 'efs-options-gates-1/x', 'data', 'efs-options-gates-1-2'):
    assert not re.fullmatch(pattern, foreign), 'harness pattern %r would own %r' % (pattern, foreign)
print('enabled-command fixture names: harness owns exactly the suffixed command/options trees it created PASS')
