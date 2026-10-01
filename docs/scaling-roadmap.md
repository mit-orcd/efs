# Roadmap — landed history and parked ideas

[Architecture](architecture.md) · [What to work on](arch/START-HERE.md) ·
[Operations](operations.md) · [Testing](testing.md)

> **The work queue lives in [arch/START-HERE.md §1a](arch/START-HERE.md).**
> That is the single home for "what do I do next". This file is history plus
> the ideas that are deliberately parked — never instructions.

## Where the scaling plan ended up

The original plan raised the inode ceiling from ~14M toward ≥ 2³² in numbered
phases over the pre-Raft engine. All of that is either landed or superseded:

| Then | Now |
| --- | --- |
| Phase 1 — make the single-table model robust | shipped |
| Phase 2 — server-owned metadata (RPC writes, then RPC reads) | shipped |
| Phase 3 / 3b — shard the table, extent-shard chunk metadata, spread hot dirs | shipped; the sharding model is now the spec's shard → Raft group → KV mapping |
| Phase 4 — slim the in-memory inode | **moot**: the in-memory whole table it slimmed no longer exists |
| Phase M — carve the monolith into `raft/ kv/ meta/ wire/ data/ client/` | complete |
| [architecture.md §10](architecture.md) steps 0–12 | all landed and gated |

So the metadata engine is Raft + an on-disk ordered KV, and the old whole-table
snapshot, CoW page flush and 2PC root commit are **deleted code**. Anything
elsewhere in the tree that describes `g_server->lock`, shard tabs,
`EFS_INO_RAM_MB`, extras catchup or `meta-rebuild` is history.

What is left is measured performance and harness work, ordered in
[START-HERE §1a](arch/START-HERE.md). Design choices the spec did not make
are listed there under "Decisions — taken and pending"; the taken ones
(D1–D13, D18–D20, D24) are implemented, the pending ones are asks.

## Parked: server-side `.stats` / `.find` refresh

Both virtual files are **user-triggered** today: `cat dir/.stats` computes
rollups on the client (TTL ~1 s), and `cat dir/.find/<term>` walks the query
subtree by RPC (result cache 5 s / 16 slots). That is the wrong trigger — a
monitoring loop pays for the walk, and a quiet tree never refreshes.

The design, if it is ever built: the **server** owns the rollups and the name
index, `cat` only reads the last committed snapshot, and a refresh fires on
either **object lag** (the snapshot's watermark is more than N objects behind)
or **time lag** (older than T seconds, so a rename-only tree still converges).
`.stats` must then report staleness for *both* snapshots — when each was built,
how far behind it is, its age, and the configured N and T — because "behind but
within budget" and "refresh overdue" are different states.

Rejected: rebuild-on-`cat`; a client-local index as the source of truth; a
full rebuild per create; funnelling a cluster-wide index through one shard.

## Parked: expiry dates (export sunset + per-inode TTL)

A product feature, not a scaling item: an object or an export carries an
absolute expiry instant, after which lookups miss it and its inode, dentries
and chunks are reclaimed. Scratch clusters want "this allocation dies on DATE"
and "files here last N days".

Decisions already made, if it is picked up:

- `expire_at` is a **separate** inode field in Unix seconds, `0 = never`, set
  only through an explicit API and compared only to **server** time. Do not
  reuse atime/mtime: atime is not maintained and mtime is user-settable, so a
  `--mtime-older` prune is a different feature and must not be conflated.
- Expired means already unlinked for any new path resolution (`ENOENT`); an
  already-open fd keeps working exactly like unlink-while-open.
- Root never expires; a directory's own expiry does not delete its children.
  Recursive sunset is an admin walk that stamps children first.
- Creates inherit the parent's `expire_at`; a late publish must not resurrect
  an expired inode.
- Ship export sunset first (one root field plus an `efs-mgmt` verb, no
  sweeper), then the per-inode field with lazy expire on lookup/open/readdir,
  then a per-shard sweeper for space. Lazy expire is the correctness path —
  it must work with the sweeper off.
- User surface is the xattr `user.efs.expire`; lookup must not need an xattr
  get to see expiry.

Rejected: client-local expiry; per-chunk expiry; sub-second/lease-style
expiry; MVCC "expired versions".

## Queued in START-HERE, not here

The Sep 28 2026 `perf` analysis of the four `efsd` and of `efs-fuse` under
`ecopy` (snapshot install on the pump thread, six `access()` per PUT,
leftover `snap-*.kvx.tmp`, client copies and reply busy-wait) is two
work-queue items with steps and gates:
[START-HERE §1a W14 and W15](arch/START-HERE.md#1a-the-work-queue). Take
them from there.

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

The spec already requires this ([architecture.md](architecture.md) §7.4,
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
  ENODEV), a ≥1 GiB single write, a many-client hardlink storm, parent
  directory mtime/ctime on create/unlink/rename/link, and directory
  `utimens` once the directory has spread across both Raft groups
  (local-directory `utimens`, spread-directory `utimens` at
  `EFS_DIR_SPREAD_MIN`, and a second client observing those times). The
  spread case is `EINVAL` today; the feature and these tests are one item
  under "Parked: cross-group directory `utimens`".
- **Invariant harnesses:** an offline `fsck --verify-only` to run after
  randomized load and after every Layer 4 case; a 300k-file single-directory
  spread stress; a fence test for leftover clients; and a stuck-catchup joiner
  case that fails the gate instead of timing out.

Contract for additions: append `@test` functions (the runner auto-registers),
use the `("ab", (fn_a, fn_b))` form for concurrent peer cases, re-run the XFS
baseline so `compare.py` sees new names, and keep crash/EC/partition out of
`posix_suite.py`.

## Rejected outright (do not implement)

Consensus-group sprawl (one group per directory), MVCC, SPDK, universal hashed
dentries for every directory, and range leases. The reasons are in
[arch/design-history.md](arch/design-history.md).
