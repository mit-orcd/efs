# Mac query delta investigation — W109 / W92

**Branch:** `devel-mac`, from devel `48a2719d`. **Cluster:** `mac` (three
Fedora aarch64 storage VMs and gateway on this Mac), buffered I/O/TCP.
This is neither native-IB acceptance nor an ORCD workload rerun.

## Confirmed cause and changes

The production disk query builder reproduces the exact 1,024-change cliff on
the Mac seed. A private KV fixture has 6,000 existing regular files and two
directories. Editing existing inode rows isolates changed-ID cardinality from
creation/parent updates. Every result is compared with an independent RAM
catalog, including all file stats and directory aggregates.

| Unique edited files | Baseline rows refolded | Fixed rows refolded |
| --- | --- | --- |
| 1,023 | 1,023 | 1,023 |
| 1,024 | 1,024 | 1,024 |
| 1,025 | 6,002 | 1,025 |
| 4,096 | 6,002 | 4,096 |

With 60,000 files, the baseline refolds all 60,002 rows at 1,025 and 4,096
changes; the fix refolds only the edited rows. This establishes the code-path
cause independently of ORCD's aggregate logs. It does not establish that every
ORCD rebuild takes this path or that it causes every foreground timeout.

- `7c3b48d2`: replace the namespace-limited changed-ID array with a transient
  candidate journal, using 256-ID buffers. Account journal bytes against disk
  admission, check partial I/O and cancellation, remove it before publication,
  and preserve errors instead of converting them to full rebuilds. IDs are
  visited once in the candidate stream and once only for old-only removals,
  so no namespace-sized deduplication allocation is required.
- `dadbb2ef`: compare sorted old/candidate inode streams with sequential merges,
  removing repeated binary lookups during revised-partition and removal scans.
  Current and retained generation isolation remains unchanged.
- `ff5f1c87`: collect removed IDs in the first merge pass rather than scanning again.
- `408043b6`: prove unchanged regular-file stat inputs by comparing exact active-lane, APPEND_CUR and conservative PVER values, instead of invalidating every file on an unrelated partition edit. Lane-only mutation/removal still invalidates correctly.
- `f02c4fae`: skip exact lookups when validated old/new optional stat families are both empty; an absent active lane still refuses reuse.
- New regressions cover the boundary, 4,096 updates, 2,048 creations and deletes,
  stat/rollup equality and journal short-write/ENOSPC refusal. Existing rename,
  hard-link, append-frontier, cancellation, cold restore and corruption/fault
  gates remain required and pass in the targeted seed run.

**Work reduction proven, latency improvement limited:** the final lane-free
60,000-file individual samples refold 1,025/4,096 rows in approximately 73/149 ms,
versus full-baseline 60,002-row refolds in 80/79 ms. The 4,096-change incremental
case can remain slower on this simple fixture. These are individual samples
with other VM activity, not a controlled performance verdict.

A second invalidation cause was reproduced with 6,000 lane-bearing files:
editing 1,023 / 1,024 / 1,025 rows refolded all 6,000 files even below the former
changed-ID limit. The exact-dependency fix refolds only the edited files.
Lane-only EOF/allocated-chunk changes and removal each refold exactly one file
and agree with the independent RAM catalog. Representative sustained live data
and repeated samples remain necessary before claiming lower foreground latency.

The 1,024 **dirty-directory** bound and missing-directory coverage fallback
remain. This repair removes the changed-inode cardinality cliff, not all full
rebuild causes or the complete W109 contention package.

## Deployment and retained-data blocker

The journal and first sequential-merge changes were built on the Mac seed;
full `make test` passed and those binaries were shipped to all three storage
VMs and gateway. Later focused query fixes passed the targeted seed oracles;
a fresh full gate passed on the seed using data-disk fixture space, and the latest
query code is deployed to all Mac service nodes (see the checkpoint below). The cluster uses
`NODE_QUOTA=10G`: its former explicit 15G exceeds the current safety estimate
(12.09 GiB on node 1). Cluster-script commit `386c710` permits a per-run quota
override. Existing stores were preserved; no wipe or forced write discard.
Build stamps from these rsync deployments say `unknown` because their tree lacks
Git metadata; running executable byte checks and source hashes establish the
actual deployed build, not that stamp.

The retained Mac namespace has an independent query bootstrap failure:
`query-disk: unlinked directory ino=4091 parent=2098 nlink=2`, followed by
`query-refresh: rc=-1 initial=1`. It also appears in October 10 logs, before
this repair. Its historical build attempts take roughly 231–248 seconds.
It prevents an installed-cluster incremental-query comparison until diagnosed
and repaired with authoritative namespace evidence. Do not delete the inode,
invent a dentry or suppress catalog validation to make it publish.

Fresh private fixture gates on the Mac seed validate query/RPC/two-FUSE
semantics; those use four local daemons and do not imply four physical hosts.
The installed three-VM native/peer POSIX gates are recorded separately.

## Evidence and checkpoint

[Boundary baseline](../../results/measure/20261011-mac-query-delta/baseline-6000.txt),
[60,000-file baseline/journal comparison](../../results/measure/20261011-mac-query-delta/journal-60000.txt),
[sequential merge and existing query oracles](../../results/measure/20261011-mac-query-delta/merge-oracles.txt),
[large creation/deletion oracle](../../results/measure/20261011-mac-query-delta/create-delete-oracle.txt).

## Live verdicts and retained evidence

Fresh private four-daemon/TCP fixture run1 passed initial catalogs, quiet metadata
refresh, partial-host refusal, RPC/JSON predicates, two FUSE mounts, permissions,
pages and streaming exports. Optional virtual-interface checks were 7 pass /
3 fail: FD capacity/recovery expected at least 20 but observed 3; deep indexed
name-limit convergence failed. These failures remain open. Failed private runs
have been unmounted and their owned processes stopped; stores/logs are retained.

The 1,536 payload-file run2 and 1,025 metadata-only run3 each exceeded the
compound fixture's 300-second deadline during creation (roughly 2–3 files/sec).
Neither is a passed large live delta gate. The deterministic 6,000/60,000-file
oracles are separate from this end-to-end liveness obligation.

Installed native POSIX initially reported 47/216 pass, 6 fail and 163 NOTRUN;
peer gates were not reached. The isolated October 11 12:53 UTC rerun reported **136/216 pass, 17 fail,
63 NOTRUN** in 254.2 seconds. Failures include directory/name/many-file timeouts
and concurrent append worker survival; subsequent worker checks show no tester
process remains. Its retained tree is `/mnt/efs/posix-efsgw-uapieh9s`.
Earlier apparent progress was baseline output, not a proof that EFS passed.

Read-only management inspection at 12:53 UTC confirms inode 4091 still has
mode 040755/nlink 2. Parent 2098 lists `level_4`, but authoritative lookup maps
that name to inode 8187, also a live directory. Inode 4091 lists no children.
This establishes a retained linkage inconsistency; it does not identify the
historical mutation that created it. No installed row/dentry was modified.
The installed query worker retries the same invalid namespace, with failed
builds of 236,526 and 248,576 ms in the 12:51–12:55 UTC window. Contention
causality and the expensive phase still need attribution.

## Two-hour continuation

Authorized Mac-only work runs from 2026-10-11 12:52:50 to 14:52:50 UTC. Default
remains buffered/TCP. No other cluster, merge or push is authorized by this run.
The tested empty-family optimization is committed; fallback diagnostics and a
dirty-directory boundary oracle are being validated. Installed service remains
up while isolated POSIX runs; deploy/restart follows clean fixture termination.

W109 remains open for dirty-directory coverage, representative performance,
retained Mac namespace diagnosis and original ORCD load/fault gates; W110 is
unchanged. See [ORCD investigation](orcd-communication-hypotheses-20261011.md).

### 13:00 UTC checkpoint

- `1533fb50` adds explicit `dirty-directory-budget` versus `missing-directory`
  fallback logs and a production-builder/RAM oracle in the default unit gate.
  On the Mac seed: 1,023 changed directories + root stay incremental (1,023
  refolds); 1,024/1,025 + root exceed the 1,024 closure budget and refold all
  7,027 rows. All directory stats and root aggregates match the RAM oracle.
  The bound remains an open scaling limitation; this commit does not remove it.
- Separate 15-second installed perf capture contains only **20 CPU samples**;
  it is insufficient to rank query hot paths. The subsequent syscall summary
  overlaps graceful shutdown, including 13,477 unlinkat calls, so it cannot
  attribute normal build latency. Preserve it as an inconclusive capture, not
  a repair justification. Raw perf stays on Mac efs2 under
  `/data1/mac-query-profile-20261011`.
- Installed Mac drained normally with no forced discard. Full latest build /
  unit gate / shipping is running. A focused fresh query smoke will run before
  service restoration, then native and peer gates run separately.

### Capacity-fixture repair

Latest deployment's full unit gate stopped in `test_storage_capacity.py`:
Mac seed `/tmp` is a 1.9 GiB tmpfs with 929 MiB available, below production's
1 GiB reserve. The refusal is correct; the test assumed arbitrary temporary
filesystems must admit a quota. Controlled inventory now tests duplicate roots,
shared usage, two devices and reserve refusal deterministically; real filesystem
capacity/failure checks remain. No production quota policy was weakened.
Successful `test_query_delta` fixtures now clean their own exact mkdtemp roots
with physical traversal; failures remain for diagnosis. Both revised tests pass
on Mac. The full unit retry is running, service still cleanly stopped.

### Full gate and shipping

Full Mac seed `TMPDIR=/data1/mac-unit-fixtures-20261011 make test` passed after
the fixture correction. Mac tooling commit `53d44ee` makes future deployments
use `/data1/efs-unit-tests` for live startup fixtures and defaults to the already
validated 10G quota. Directory setup/remote identity quoting and shell syntax
checks passed. All latest EFS daemon/FUSE binary hashes match on efs1/2/3/efsgw.
The focused 64-file metadata-only smoke is running in fresh private run4, with
existing RPC/Raft diagnostics. This is explicitly below the changed-ID boundary
and cannot replace the outstanding 1,025-file live gate. Service restoration
follows fixture cleanup; no forced discard, installed metadata edit or wipe.

### Live smoke and cleanup diagnosis

Run4's metadata update phase failed a new helper's early assertion: file counts
were unchanged while an older published snapshot reported transient refresh BUSY.
The helper now waits within its original 120-second deadline for complete,
error-free, exact counts, and still independently verifies every indexed mtime
and size against the requested mutation and peer syscall state. Run5 passed
64-file create/update/delete/rename/hard-link checks, normal RPC/command/page
gates and cold catalog restore, with root oracle 277 files / 7 dirs / 2,451 bytes.
No large-delta gate is inferred from this smoke.

Run4 also exposed a pre-existing fixture lifecycle error: Popen tracked FUSE's
daemonizing parent, so waiting on it did not prove the real client had drained
before servers were stopped. One unmounted client subsequently logged metadata
flush/data-loss warnings; it finally exited, and its fixture evidence is kept.
This does not prove payload loss (run4's new payloads are empty), and its cleanup
is not a clean gate. The harness now launches foreground clients and uses normal
stop-control drain while servers remain available; refusal preserves the private
servers/clients instead of killing an unresolved client. Fresh run6 validates
this lifecycle change. Installed Mac service was restored before run6, buffered
TCP; no private process from run4/run5 remained at its start.

A production-builder 64-parent/lane-bearing fixture passed before any alias
optimization: 1,023/1,025 updates took individual stats-phase samples of 128/126
ms; 4,096 took 463 ms. These samples are not controlled end-to-end latency
measurements. The existing per-inode/per-shard alias lookup order may churn the
small FD cache across many parent shards; the next step is a matched syscall
comparison before changing that traversal.

### Foreground/drain acceptance

Run6 passed the 16-file smoke, RPC/command/options/pages gates and cold catalog
restore (253 files / 7 dirs / 2,451 bytes) using foreground clients and normal
stop-control drain. This validates tracking the actual FUSE processes and
waiting for their teardown before fixture-server shutdown. No force discard is
used. Native/peer installed gates are next, separately, after query unit work.

### Alias FD-cache repair

`c09ef7f8` processes each fixed 256-ID journal batch by alias shard, rather than
reopening every alias shard for every changed inode. The 32-descriptor bound,
complete alias coverage, candidate isolation and cancellation remain. In a
controlled 64-shard/256-ID production-helper gate, identical lookup semantics
use 64 opens / 128 reads instead of 16,384 / 32,768. Immediate cancellation
opens nothing; a truncated alias record fails IO rather than requesting FULL.

All 6,000-file per-inode/RAM oracle checks and 64-parent aggregates pass, as do
creation/deletion, lane-only mutation/removal, dirty-directory boundary, rename,
retained snapshot, short-write/ENOSPC and fsync-refusal gates. Individual matched
logical-fixture stats samples for 1,023 / 1,024 / 1,025 / 4,096 edits are
128.292 / 117.518 / 126.129 / 463.091 ms before, versus
26.910 / 21.493 / 21.347 / 52.383 ms after. This demonstrates reduced cache work
and better fixture timings, not live ORCD or installed-Mac foreground latency.
The actual efsd is relinked with this code; controlled Mac rollout is running.

`04ba852d` commits the live mutation helper and foreground/drain lifecycle fix;
run5 and run6 acceptance are distinct from the original failed large live gates.


### 13:30 UTC Mac checkpoint: failed-build phase attribution

`e679adb7` adds diagnostic-only phase timing to failed query disk builds. The
Mac targeted query delta/disk/catalog/IO gates pass. The actual efsd is relinked
and shipped; installed service is restored with buffered/TCP and opt-in existing
Raft/client RPC counters. No other cluster is touched.

The first measured installed failure reports **index 261,430 ms / stats 345 ms /
persist 0 ms**, 40,939 inode rows; the same retained orphan directory rejects
publication. This rules out inode stat refolding as the expensive phase for this
attempt. It does not yet separate raw/index fsync cost from snapshot scans and
name sorting; that needs separate syscall/profile captures. RPC samples show
reply waits dominate pool checkout and sends. Observed Raft persist maxima are
roughly 69–92 ms; these are intervals, not proof of a causal mechanism.

The isolated native rerun is **137/216 PASS, 16 FAIL, 63 NOTRUN**, 244.4 seconds,
with an unchanged local-filesystem baseline of 216/216 PASS. Original deadlines
remain, and append worker survival aborts later cases. The initial peer attempt
was NOTRUN: its fixed `/mnt/efs/posix-2c` directory has historical permissions
incompatible with the test user. The cluster wrapper now uses a unique parent
for local peer mounts as it already did for remote peers. A fresh full peer run
is in progress; this fixture repair is not a passed EFS correctness gate.


### Full installed Mac peer acceptance, before raw-sync batching

The unique-parent local two-mount peer gate completed **67/67 PASS, 0 FAIL**
in 1,534.8 seconds. All 800 concurrent append records were present, complete and
unique. Append versus truncate, reverse extension completion, rename/unlink
source and destination races, hardlinks and range locks pass. The harness exited
and normally drained the secondary mount; no forced discard or killed client.
This is two independent FUSE mounts on one gateway against three Mac storage VMs,
not two physical client hosts. It does not turn the native deadline failures into
passes. A separate syscall/profile capture follows this correctness run.
