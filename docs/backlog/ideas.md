# Roadmap — landed history and parked ideas

[Architecture](../how-it-works/architecture.md) · [What to work on](../status/README.md) ·
[Operations](../operations/operations.md) · [Testing](../how-it-works/testing.md)

> **The work queue lives in [the status page §1a](../status/README.md).**
> That is the single home for "what do I do next". This file is where the
> ideas that are deliberately parked live — never instructions. The landed
> history is in [../archive/project-history.md](../archive/project-history.md).

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
| [architecture.md §10](../how-it-works/architecture.md) steps 0–12 | all landed and gated |

So the metadata engine is Raft + an on-disk ordered KV, and the old whole-table
snapshot, CoW page flush and 2PC root commit are **deleted code**. Anything
elsewhere in the tree that describes `g_server->lock`, shard tabs,
`EFS_INO_RAM_MB`, extras catchup or `meta-rebuild` is history.

What is left is measured performance and harness work, ordered in
[the status page §1a](../status/README.md). Design choices the spec did not make
are listed there under "Decisions — taken and pending"; the taken ones
are design approvals, with implementation and acceptance recorded separately;
the pending ones remain asks.

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

## Queued in the status page, not here

The Sep 28 2026 `perf` analysis of the four `efsd` and of `efs-fuse` under
`ecopy` (snapshot install on the pump thread, six `access()` per PUT,
leftover `snap-*.kvx.tmp`, client copies and reply busy-wait) is two
work-queue items with steps and gates:
[W14](work-items.md#w14--server-snapshot-install-and-the-fragment-probe-are-on-the-write-path)
and
[W15](work-items.md#w15--client-copies-and-busy-waits-are-the-write-cpu)
in work-items.md. Take them from there.

## Directory `utimens` — implementation and gate scope

The Sep 28 EINVAL observation and old source diagnosis are
[preserved](../archive/parked-testing-checkpoint-20261007.md). They are not the
current source behavior: `host_utimens` now sends the timestamp-generation
fence to used lanes in the other group before publishing the inode update,
forwarding to a host with both groups when necessary. Unit/model directory
mtime-generation checks also exist. Source presence does not establish the
full cross-group, two-client explicit-utimens acceptance gate.

The accepted directory-time row 0m/W57 evidence concerns parent timestamps
on namespace mutations; do not reopen that closed row or treat it as blanket
acceptance of every explicit directory-time operation. Reconcile named tests
and build-specific results before declaring the distinct cases below passed.

**Gate checklist (inspect current runner before adding tests):**

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

POSIX layers 1–3 exist; test counts change with the build, so use runner
listing and the recorded results rather than the old 201/63 inventory. They
cover syscalls, peer visibility and same-file races. The following is a
historical candidate-gate checklist, not proof that every test is still absent:

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
  old EINVAL diagnosis is superseded by the source update above; cross-group
  explicit-utimens gate evidence must be checked separately. Parent directory
  mtime/ctime acceptance on namespace mutations is recorded as closed row 0m,
  including its retained single-client/peer results; it is not an open task here.
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
[design-history.md](../archive/design-history.md).
