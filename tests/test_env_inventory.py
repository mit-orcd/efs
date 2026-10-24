#!/usr/bin/env python3
"""A6: every EFS_* setting the product reads has an inventory row.

1. scripts/env_inventory.py --check passes on this tree against
   docs/status/a6-settings-inventory-20261024.md.
2. Negative controls on a copy of src/, include/ and the inventory: a new
   reader through a two-level name-taking helper, a reader through a macro,
   a new compile-time mode, a fault hook outside #if EFS_FAULTS, a hook only
   under EFS_FAULTS without the EFS_FAULT_ prefix and a removed setting read
   again each fail the check, naming the variable.

Usage: python3 tests/test_env_inventory.py (no build needed)"""
from pathlib import Path
import shutil, subprocess, sys, tempfile
root = Path(__file__).resolve().parents[1]
script = root / 'scripts/env_inventory.py'
doc = 'docs/status/a6-settings-inventory-20261024.md'


def check(tree):
    return subprocess.run([sys.executable, str(script), '--root', str(tree), '--check', str(tree / doc)],
                          capture_output=True, text=True, timeout=60)


p = check(root)
assert p.returncode == 0 and p.stdout.rstrip().endswith('PASS'), p.stdout[-2000:]
with tempfile.TemporaryDirectory(prefix='efs-env-inventory-') as d:
    tree = Path(d)
    for sub in ('src', 'include'):
        shutil.copytree(root / sub, tree / sub)
    (tree / doc).parent.mkdir(parents=True)
    shutil.copy(root / doc, tree / doc)
    assert check(tree).returncode == 0
    (tree / 'src/common/zz_inventory_probe.c').write_text(r'''
#include <stdlib.h>
#define PROBE_NAME "EFS_PROBE_MACRO"
static unsigned long probe_raw(const char *name)
{
    const char *v = getenv(name);
    return v ? strtoul(v, NULL, 10) : 0;
}
static unsigned long probe_knob(const char *what, const char *name)
{
    (void)what;
    return probe_raw(name);
}
#if EFS_PROBE_MODE
static int probe_mode;
#endif
unsigned long probe_all(void)
{
    unsigned long n = probe_knob("x", "EFS_PROBE_HELPER") + probe_raw(PROBE_NAME);
    n += getenv("EFS_FAULT_PROBE_OPEN") != NULL;
    n += getenv("EFS_PACK_CRASH") != NULL;
#if EFS_FAULTS
    n += getenv("EFS_PROBE_GATED") != NULL;
#endif
    return n;
}
''')
    p = check(tree)
    assert p.returncode == 1, p.stdout[-2000:]
    for want in ('EFS_PROBE_HELPER is read by the product but has no entry',
                 'EFS_PROBE_MACRO is read by the product but has no entry',
                 'compile-time mode EFS_PROBE_MODE',
                 'fault hook EFS_FAULT_PROBE_OPEN is read outside #if EFS_FAULTS',
                 'EFS_PROBE_GATED exists only under EFS_FAULTS',
                 'removed setting EFS_PACK_CRASH is still read'):
        assert want in p.stdout, (want, p.stdout[-2000:])
print('A6 inventory check: tree PASS; helper, macro, mode, fault-gate and removed-read controls FAIL as expected PASS')
