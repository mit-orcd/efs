#!/usr/bin/env python3
"""Task FB: scratch variant of tests/test_k41_packer_placement.py with a small
step-2 batch, so placement misses node 6 often enough to observe.

Usage: k41_forced_miss.py TREE FILES MAX_BATCHES OUT
  TREE         a built tree holding the committed test (fd50deb31 or later)
  FILES        step-2 files per batch (60 in the real test; 10 packs about one segment)
  MAX_BATCHES  1 reproduces the pre-fix single batch; 4 is the committed fix
  OUT          path of the variant to write inside TREE/tests (it imports k33_daemons)
A run that fails `refused >= 1` reports node 6's probe failures: 0 means
placement never chose node 6. Run it as `python3 OUT` from TREE.
"""
import sys
from pathlib import Path
tree, files, maxb, out = Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), Path(sys.argv[4])
s = (tree / 'tests/test_k41_packer_placement.py').read_text()
def sub(a, b):
    global s
    assert s.count(a) == 1, f'anchor not found once: {a!r}'
    s = s.replace(a, b)
sub("        names3 += batch(a, f'refuse{rounds}')\n",
    f"        small = [f'refuse{{rounds}}-{{k}}' for k in range({files})]\n"
    f"        for k, name in enumerate(small): write(a, name, SIZES[k % len(SIZES)])\n"
    f"        names3 += small\n")
sub("        wait_packed(c, base3, BATCH, f'node 6 refusing, batch {rounds}')\n",
    f"        wait_packed(c, base3, {files}, f'node 6 refusing, batch {{rounds}}')\n")
sub("        if refused or rounds == 4:\n", f"        if refused or rounds == {maxb}:\n")
out.write_text(s)
print(f'wrote {out}: step 2 batch {files} files, at most {maxb} batches')
