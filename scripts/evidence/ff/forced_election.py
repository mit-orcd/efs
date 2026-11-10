#!/usr/bin/env python3
"""Task FF (K104): the gate step of tests/test_ao_readdir_live.py with node 3
forced to win group 2's first election.

Usage: forced_election.py TREE   (TREE built with `make all tests/faults/efsd`;
TMPDIR holds the retained fixture, as for the gate test)

Loads TREE's own scripts/evidence/ao/qualify_ao_readdir.py (so each tree keeps
its own environment names) and inserts one line before the stable files are
written: `k.place_leaders(c,3,groups=(2,),timeout=15)`. The rest of the step
is unchanged: leaders move to node 1 and then node 2, node 3 gets SIGSTOP,
four listings, SIGCONT. This is the election outcome of the wave 70 failure
and of every reproduction; without it most runs never give node 3 group 2."""
import os, sys, tempfile, time, types
from pathlib import Path
from types import SimpleNamespace
tree = Path(sys.argv[1]).resolve()
path = tree / 'scripts/evidence/ao/qualify_ao_readdir.py'
src = path.read_text()
anchor = "        target=mount.path/'ao-stable';target.mkdir();ino=target.stat().st_ino"
assert src.count(anchor) == 1, 'driver layout changed: anchor not found once'
src = src.replace(anchor, "        k.place_leaders(c,3,groups=(2,),timeout=15)\n" + anchor)
mod = types.ModuleType('qualify_ao_forced'); mod.__file__ = str(path)
exec(compile(src, str(path), 'exec'), mod.__dict__)
fixture = Path(tempfile.mkdtemp(prefix='efs-ao-live-', dir=os.environ.get('TMPDIR')))
started = time.monotonic()
try:
    mod.run(SimpleNamespace(work=fixture / 'work', output=fixture / 'evidence',
                            transport='tcp', bind='127.0.0.1', base_port=None, posix=False))
finally:
    print('AO retained fixture', fixture, 'elapsed_s', round(time.monotonic() - started, 3), flush=True)
