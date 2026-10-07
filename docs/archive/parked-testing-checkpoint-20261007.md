# Parked testing checkpoint — superseded wording

Historical source/test claims retained, not a current implementation inventory.
Use the [current parked ideas](../backlog/ideas.md) and queue acceptance records.

## Parked: cross-group directory `utimens`

`futimens` / `os.utime` on a directory returns `EINVAL` when that directory
is not `EFS_META_LAYOUT_LOCAL` and one of its `used_shards` sits in the
other Raft group. `host_utimens` bails with `EFS_ERR_INVAL` and the comment
`cross-group lane fence later` (`src/server/raft_host.c`). The client maps
that to `EINVAL`. A local directory and an ordinary file already succeed
(`attr_utimens` / `attr_utimens_ns` are files only).

Seen from `ecopy` restamping directories under `/tmp/efs-mount/knouse/`
(Sep 28): `futimens: Invalid argument` on directory paths. The copy
continues; only those timestamps are skipped.

The spec already requires this ([architecture.md](../how-it-works/architecture.md) §7.4,
directory `utimens`): `dir_mtime_gen` on the directory row, bumped only by
`utimens`, distributed to the used lanes by the same bounded fence a file
uses (≤65 authorities). Do not leave the `EINVAL` as the behavior.

**POSIX tests, in the same change** (runner auto-registers `@test`):

- `posix_suite.py`: `utimens` on a directory that is still local (a handful
  of children). `mtime` and `atime` stick, including nanoseconds. Must not
  return `EINVAL`.
- `posix_suite.py`: the same on a directory that has spread. Spread starts
  at `EFS_DIR_SPREAD_MIN` (65536 names, `include/efs/common.h`), which is
  what puts `used_shards` in both groups. `futimens` returns 0 and
  `stat` shows the times that were set.
- `posix_2client.py`: after that spread-directory `utimens`, the other
  client sees the same `mtime` and `atime` (remount or a fresh lookup, not
  a same-mount dcache hit).

Re-run the XFS baseline so `compare.py` sees the new names. Do not weaken
the file `utimens` tests to make the directory case pass.

## Parked: tests still to write

POSIX layers 1–3 exist (`tests/posix/posix_suite.py` 201 tests,
`posix_2client.py` 63). They cover
syscalls, peer visibility and same-file races. Not covered:

- **Layer 4 fault injection** (`tests/faults/` is the start of it, not in
  `make test`; see `tests/faults/README.md`): crash after `fsync`; kill
  between EC/publish stages; degraded read and heal with one node down; silent
  checksum repair of a corrupted fragment; partition/fencing, including a
  leftover client that must not publish onto a fresh `mkfs`; lock-holder crash;
  kill mid-rename on a spreading directory. Layer 4 is efs-specific — an XFS
  PASS row is not the oracle.
- **Deterministic races.** The existing race tests fork and hope; they need
  barriers so the two operations actually collide, and
  `trunc_zero_then_high_pwrite` needs a multi-chunk original so it exercises
  stale resurrection across 128 KiB boundaries.
- **Missing POSIX cases:** a second uid (most `perm_*` tests no-op as root),
  cross-client `MAP_SHARED` (or document it as unsupported — it is currently
  ENODEV), a ≥1 GiB single write, a many-client hardlink storm, and directory
  `utimens` once the directory has spread across both Raft groups
  (local-directory `utimens`, spread-directory `utimens` at
  `EFS_DIR_SPREAD_MIN`, and a second client observing those times). The
  spread case is `EINVAL` today; the feature and these tests are one item
  under "Parked: cross-group directory `utimens`". (Parent directory
  mtime/ctime on create/unlink/rename/link left this list Oct 6 — it is
  queue row 0m with tests in tree; a spread-directory mtime case is still
  owed, see row 0m.)
- **Invariant harnesses:** an offline `fsck --verify-only` to run after
  randomized load and after every Layer 4 case; a 300k-file single-directory
  spread stress; a fence test for leftover clients; and a stuck-catchup joiner
  case that fails the gate instead of timing out.

Contract for additions: append `@test` functions (the runner auto-registers),
use the `("ab", (fn_a, fn_b))` form for concurrent peer cases, re-run the XFS
baseline so `compare.py` sees new names, and keep crash/EC/partition out of
`posix_suite.py`.
