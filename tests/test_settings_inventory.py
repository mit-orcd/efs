#!/usr/bin/env python3
"""S4/W122: the settings table has one row for every A6 inventory entry of
the two listed kinds, so a new setting without a row fails here.

Reads docs/status/a6-settings-inventory-20261024.md (the table `make test`
already keeps complete against the readers: tests/test_env_inventory.py) and
src/common/settings.c; the header counts of the kinds the listing leaves out
must equal the inventory's. Negative control: a table without one row fails."""
import re, sys
from pathlib import Path
root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'scripts'))
import env_inventory  # noqa: E402

kinds = env_inventory.inventory_kinds(str(root / 'docs/status/a6-settings-inventory-20261024.md'))
listed = {n for n, k in kinds.items() if k in ('deployment input', 'numeric tunable')}


def table(text):
    return set(re.findall(r'^\s*R\("([^"]+)"', text, re.M)) | \
        set(re.findall(r'\bR\("([^"]+)"', text))


def check(text):
    rows = table(text)
    assert {r for r in rows if r.startswith('EFS_')} == listed, \
        (sorted(listed - rows), sorted(r for r in rows if r.startswith('EFS_')) and sorted(rows - listed))
    assert '--writers' in rows and len(rows) == len(listed) + 1, rows


src = (root / 'src/common/settings.c').read_text()
check(src)
header = (root / 'include/efs/settings.h').read_text()
for macro, kind in (('EFS_SETTINGS_OBSERVABILITY', 'observability'), ('EFS_SETTINGS_FAULT_HOOKS', 'fault/test hook')):
    n = int(re.search(rf'#define {macro} (\d+)', header).group(1))
    assert n == sum(1 for k in kinds.values() if k == kind), (macro, n)
# Negative control: dropping one row, or adding a setting to the inventory only, fails.
for broken in (src.replace('R("EFS_RDMA_PORT"', 'X("EFS_RDMA_PORT"'),):
    try:
        check(broken)
    except AssertionError:
        continue
    raise SystemExit('a table without EFS_RDMA_PORT passed')
listed.add('EFS_NEW_TUNABLE')
try:
    check(src)
except AssertionError:
    pass
else:
    raise SystemExit('an inventory entry without a row passed')
print(f'settings table: {len(listed) - 1} inventory rows + --writers, header counts match PASS')
