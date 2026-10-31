# Memcheck on the ORCD hosts without root, October 31

**Purpose:** make the two Memcheck-based gate tests (`tests/test_rdma_memcheck.py`, `tests/test_leak_gate.py`) run on ORCD, for the NOTRUN cells of the [Stage 1 acceptance matrix](stage1-acceptance-matrix-20261030.md) (K26 ORCD leak check, row O9). Task BU. **Verdict: PASS** (both tests ran to a result on fstor007; no product or test change was needed).

## Result in one paragraph

A user-prefix valgrind 3.28.0 in `$HOME/.local/valgrind-3.28.0` is enough. The tests already find it through `PATH` (the binary), `CPATH` or `C_INCLUDE_PATH` (the header, through the `cc` default search) and `EFS_VALGRIND` (leak gate only). Nothing in the tests or Makefile changed. `/home` is one NFS filesystem on fstor007, node9900 and the login node, so this is **one install for the fleet**.

## Tested source and environment

- Source: devel `909a07d71`, shipped once with `git archive` to `/tmp/efs-taskBU-tree` on fstor007 (removed after the run).
- fstor007: RHEL 9.7, gcc 11.5.0, glibc 2.34, 96 CPUs, mlx5_0..mlx5_3 (MT4129) all Active, `EFS_RDMA_DEV=mlx5_0`. System `/usr/bin/valgrind` 3.26.0 (rpm `valgrind-3.26.0-5.el9`) is installed, `valgrind-devel` is not, so there is no `valgrind/memcheck.h` anywhere. No root.
- node9900 (read-only look): RHEL 9.8, glibc 2.34, no valgrind on `PATH`, no `/usr/include/valgrind`. node9900 shows the same `/home` (marker file written on fstor007 is visible there) and `$HOME/.local/valgrind-3.28.0/bin/valgrind --version` prints `valgrind-3.28.0`. Nothing was written on node9900.

## Build and checksum

Tarball `https://sourceware.org/pub/valgrind/valgrind-3.28.0.tar.bz2` (17,182,495 bytes, release of 23 Oct 2026; 3.28.0 is the "current" release on valgrind.org). Published values from `https://valgrind.org/downloads/current.html`, fetched on the same host, matched the downloaded file:

| | value |
|---|---|
| md5 (published = computed) | `2bfffc2585eba66a354816cc1ee87ffb` |
| sha1 (published = computed) | `ed9297806b5d35ef3e885ca5efbe507a66b37007` |
| sha256 (computed, not published) | `9f7a18d128f05d3a80fea53e6953f34aad7200a425ceaee46099a8a798a7dd9c` |

The PGP signature (`.asc`) was downloaded but not verified (no key import). A 3.26.0 tarball downloaded first was discarded unused: valgrind.org publishes no checksum for it. Build: `./configure --prefix=$HOME/.local/valgrind-3.28.0 && make -j16 && make install`, in `screen`, rc 0, about 2 minutes.

## How the gate finds it

- `tests/test_rdma_memcheck.py`: `shutil.which('valgrind')` (so `PATH`), then compiles with `$CC` (default `cc`) and prints NOTRUN when the build error names `valgrind/memcheck.h`. `cc` honours `CPATH`. Without the header the run prints `NOTRUN: RDMA Memcheck gate: valgrind/memcheck.h is not installed` (confirmed on fstor007 with the system valgrind).
- `tests/leak_check.sh` (`make leak-check`, and the fixtures of `test_leak_gate.py`): `EFS_VALGRIND` or `valgrind` on `PATH`; NOTRUN exit 3 otherwise.
- `tests/valgrind_leaks.sh` additionally takes `EFS_VG_RDMA_RUNTIME` (annotated verbs runtime); not used here.

## Results (fstor007)

| test | result | duration |
|---|---|---|
| `tests/test_rdma_memcheck.py` | PASS: bounded completion, repost/tail poisoning, undefined send detection (positive run exit 0, negative run exit 99 with `client check request`) | 1 s |
| `tests/test_leak_gate.py` | PASS: 7 fixtures (missing-valgrind NOTRUN control, no report, clean, leaking, Python-wrapped leak, crash child, `VAR=value`, failing test) | 2 s |

Baseline with no environment (system valgrind 3.26.0, no header): `test_rdma_memcheck` NOTRUN (header), `test_leak_gate` already runs its real-Memcheck fixtures and passes, so on fstor007 the leak-gate fixtures were never the blocker; the header was.

Suppressions and false positives: **none** appeared. No leak, invalid access or uninitialised-value report outside the tests' own deliberate negative fixtures, so there is no K-row candidate.

## Requirement per ORCD host (for the matrix cells)

```bash
export PATH=$HOME/.local/valgrind-3.28.0/bin:$PATH
export CPATH=$HOME/.local/valgrind-3.28.0/include
export EFS_RDMA_DEV=mlx5_0        # RDMA cells only
```

- Install prefix: `$HOME/.local/valgrind-3.28.0` (the home of the agent account, shared NFS `/home`). One install serves fstor007, node9900, node9901 and the login node of this account; the fcstor fleet was not checked for the same home (check `ls $HOME/.local/valgrind-3.28.0/bin/valgrind` there first). The binary built on RHEL 9.7 (glibc 2.34) runs on the 9.8 node9900.
- Memcheck cannot execute AVX-512: `tests/valgrind_leaks.sh` already rebuilds with `-mno-avx512f` when the host has `avx512f` (see [sanitizer host limits](../how-it-works/testing.md)).
- Not done here: `make leak-check` itself (K26 row O9) and the live phases of `tests/valgrind_leaks.sh` on ORCD; they are now unblocked by the environment above and still owed.

## Reproduction and raw locations

```bash
# on fstor007 (ssh -J orcd-login fstor007), after the build above
git archive 909a07d71 | ssh fstor007 'mkdir /tmp/efs-taskBU-tree && tar xf - -C /tmp/efs-taskBU-tree'
bash ~/efs-artifacts/taskBU-20261031/run.sh    # prints which valgrind, then both tests with seconds
# cleanup: ~/bin/edelete --force --delete /tmp/efs-taskBU-tree
```

Raw files stay on fstor007 (shared home) in `~/efs-artifacts/taskBU-20261031/`: `run1.log` sha256 `1288845a3c933d6ce1c74709e0ac988c0e637ab7e6884c137bce4fe3a5c56720`, `build.log` sha256 `9c03105fd660c41712dc2088010d931141601265bb8bdee6f07761dc30089081`, `run.sh` sha256 `5c6ebb6b5dc0c21590a90dc4b7d98f456ed83e743081f319cbf13c6cbe690c80`, and the verified tarball under `dl/`.

## Limits

One host, one run of each test. `make leak-check` and the daemon phases were not run. The extracted source tree and `/tmp` tree were removed with `rm -rf` before this page was written (not `edelete`); only the agent's own files were touched.
