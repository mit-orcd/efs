# Include boundary check (W171 phase 3)

[Include boundary check (W171 phase 3)](../include-boundary-20261104.md): **PASS** (Nov 4): `scripts/check_include_boundary.py` runs in `make docs-check`, counts `#include "../<dir>/"` per (including dir, included dir) pair against an allowlist of the 22 existing includes (8 pairs), fails on a new pair or higher count; `--selftest` in `make test`. No product code, no K row.
