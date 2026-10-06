# Project status — the task right now and the work queue

[Architecture](../how-it-works/architecture.md) ·
[Backlog](../backlog/README.md) · [In flight](in-flight.md) ·
[Decisions](decisions.md) · [Operations](../operations/operations.md)

This page exists because of the bar in [developing.md](../how-it-works/developing.md): **a
less advanced model must be able to contribute a correct change.** That is
only true if finding *the next task* and *the exact pages that govern it* is
mechanical. Read this page, then read at most the two or three files it
sends you to — not the whole spec.

---
## 1. The task right now

**Current implementation task — D25 writer integration (Oct 6).**
Memory/recovery fixes are committed; the dated checkpoints below retain their
original working-tree status and are superseded by the review checkpoint.
Partial flushes now rebuild from a fresh authoritative masked view even when
the base object generation is unchanged. Missing rows use a zero merge base
and empty-row CAS; failed read authority preserves the local dirty body and
returns its actual error. Both pipelined and direct flush paths carry the
captured byte observation into publication. See the current
[in-flight handoff](in-flight.md) for the remaining writer/sweep sequence.


**Metadata scaling gap (Oct 5).** The implementation still maps logical
metadata shards into two fixed Raft groups and routes REPORT batches through
nodes hosting both groups. The architecture calls for independent shard groups
with leadership distributed across nodes. This needs implementation work before
claiming metadata capacity for 100–1,000 active clients; connection count alone
does not establish capacity. The [metadata scaling plan](metadata-scaling.md)
records the code evidence, routing/runtime changes, hotspot handling and gates.
Open follow-up after correctness work, including W43/D25 and D27/0a; recording
this plan does not enable or deploy a new topology.

**W38 checkpoint (Oct 5).** STALE replay now preserves live spans even when
the base still names this client's object. Full folds no longer infer byte
coverage from a later metadata-only list; chain-full conversion requires a
captured byte observation. Production-function tests reproduce the old replay
byte loss and pass with the fix, including ASan/UBSan. The
[W38 backlog entry](../backlog/work-items.md#w38--ior-hard-fold-tombstone-without-the-spans-bytes-queue-row-0e)
records remaining traced IOR/cold-hardscan gates and the possible STALE return
on an unsafe conversion. Uncommitted; no cluster rollout. Next code item:
**W43/D25**, durable history and production read/publish/sweep integration;
its metadata primitives are now implemented and locally tested; production integration remains open.

**0j checkpoint (Oct 5).** Local FUSE mutations now invalidate the lookup
memo; active operations suppress memo use and a serial rejects older LOOKUP
replies. Production-function/callback regressions, ASan/UBSan and earlier
memory tests pass locally. The
[0j backlog entry](../backlog/work-items.md#0j--the-clients-50-ms-lookup-memo-returns-pre-mutation-stats-queue-row-0j)
records conservative whole-memo invalidation and pending Linux posix/Spark du
gates. Changes remain uncommitted. The following **W38** byte-observation fixes are now in tree too; see below.

**W54 checkpoint (Oct 5).** The next item in the agreed sequence was W54;
its apply fix and regression tests are now in tree with local gates passing.
The [W54 backlog entry](../backlog/work-items.md#w54--a-folds-gc-deletes-the-live-base-queue-row-0i)
records the change and the outstanding cluster gate. The following **0j** memo fix is now in tree too; see its checkpoint below. W54 is not
marked fully gated until the post-GC cold-read check passes on the cluster.

**Concurrent create/mkdir errors (Oct 6, local and uncommitted).** The
`testing` screen on xefsgw stopped on EIO creating
`tiny_files6/folder_000278/level_2`, under a million-file/16-worker workload.
The new FUSE process remained alive at about 278 MiB RSS; this run was not an
OOM. Server logs showed BUSY replies and apply lag, but do not establish the
exact final failure path. Inspection found that create/mkdir/symlink read a
shared `g_client.last_err`, allowing another request to overwrite the error.
`efs_client_create_result` now returns each request's error and inode directly;
all FUSE create/mkdir/symlink callers consume that result. BUSY remains EBUSY,
quota remains ENOSPC, and EXIST visibility checks preserve lookup failures.
The compatibility inode-only wrappers remain for existing non-FUSE callers.
A deterministic 16-thread regression exercises overlapping success/BUSY/quota
results and visibility-error mapping, and is included in `make test` as
`test-create-errors`. It passes locally normally and under ASan/UBSan, and
with Linux GCC on the gateway. Linux syntax checks for the full ops/FUSE sources
pass. The gateway lacks sanitizer runtime libraries; Linux sanitizer execution
and the actual workload rerun remain pending. No deployment, restart, directory
cleanup or live workload replay was performed for this fix.

**W36 follow-up (Oct 6, local and uncommitted).** A deterministic regression
reproduces a second window: unlink removes the source and its last-link inode
before rename's source PREPARE. The unversioned unlink leaves the txn version
sidecar unchanged, so the old version-only DEL accepts the absent key. The
existing BUSY probes cover only unlink after preparation. This mechanism is
present in the committed implementation too; the cluster's dirty D25 build
alone does not establish that D25 caused the recurrence.
Source capture now validates the originally observed dentry identity and
captures exact local/hashed bytes or absence. The shared server rename,
unlink and rmdir source-drop helper uses EXCL_VALUE PREPARE, rejecting deletion
or name reuse before preparation; existing BUSY probes protect prepared keys.
Split-directory copies and tombstones are handled explicitly. Actual-core
regressions reproduce the unsafe old acceptance and verify both race orders,
unlink/recreate ABA, local/hashed/splitting snapshots and a live destination
after resolution. Metadata and transaction suites pass normally and under
ASan/UBSan; deterministic simulator and strict server syntax checks pass locally.
**W36 remains open pending Linux build and the cluster's 20/20
`peer_rename_vs_unlink_src` gate, first on the next rollout.** The dangling
`b` remains untouched as evidence; no deployment or repair was performed.

**D25 implementation checkpoint (Oct 5, local and uncommitted).** The design
in [architecture.md §7.3](../how-it-works/architecture.md) and shared C masking
helper now have metadata support in `src/meta/meta_apply.c`: versioned,
FileID-scoped inode/lane history sidecars; atomic history/stamp append with
expected-epoch checks, replay and full-history BUSY; bounded coherent chunk
views with separate base/span epochs and captured masks; and a single-row
sweep CAS that atomically commits the replacement or dead-row deletion,
lane stamp and alias-safe GC records. Existing inode/lane encodings are
unchanged; absent histories decode as empty and malformed records fail closed.
Metadata regressions pass normally and under ASan/UBSan, including storage
failure, concurrent publication/fencing, read-view collection races and bounded
retry exhaustion. The independent 20,000-operation byte model and earlier
client-memory, 0j and W38 regressions pass again.

**D25 transaction checkpoint (Oct 5, local and uncommitted).** Transaction
records now support all 64 participant shards (the inode shares lane 0's
shard), full 520-byte histories and their intent envelopes. New exact-value
PREPARE compares the observed row bytes instead of relying on a version
sidecar untouched by ordinary publication. It persists the existing EXCL
intent format; matching prepared retries are idempotent, conflicting
transactions return BUSY, and changed replacements/participant lists are
STALE. Server and simulator namespace coordinators retain their previous
eight-participant/guard limit. Transaction regressions pass normally and under
ASan/UBSan: all 65 authorities, visibility before/after the decision, recovery
from an applied-state snapshot after partial resolution, replay, unreachable
coordinator, unversioned lane races, guard/reduce envelopes, malformed payloads
and durable-intent length overflow. The metadata suite passes under
ASan/UBSan and the full deterministic simulator passes locally. This is
transaction support, not a production truncate coordinator.

**D25 participant-fence checkpoint (Oct 5, local and uncommitted).**
`efs_meta_prepare_content_fence` now prepares the authority's stamp and
history as one atomic EXCL pair. An inode checks its captured active-lane
bitmap and base size, and requires every active lane shard in the participant
list; a lane checks its publication sequence and epoch. No intent is installed on a
second-key conflict, stale snapshot or failed storage batch. A fixed
big-endian CONTENT_FENCE PREPARE subtype uses the existing server/simulator
transaction decoder and verifies that the command key names the encoded
authority. All ordinary inode/lane/history writes in metadata apply now
honor EXCL intents; chunk-only sweeps honor their parent lane's intent too.
Commutative namespace reductions remain allowed. This adds local KV probes
on guarded mutations, with no coordinator RPC in ordinary apply; Linux
performance remains to be measured. Metadata and transaction regressions
pass under ASan/UBSan, strict syntax checks pass, and the deterministic
simulator passes. Tests include lane activation before/after preparation,
competing truncates, publication, setattr/utimens/legacy-truncate exclusion,
chunk-only sweep exclusion, atomic pair failure, decision visibility,
resolved masks and resumed writes. This completes participant preparation
and metadata writer exclusion, not the production shrink coordinator.

**D25 committed-reader checkpoint (Oct 5, local and uncommitted).**
Handler inode reads now resolve committed EXCL replacements even when an old
row exists. `efs_txn_read_ex` accumulates undecided decisions across a collect;
GETATTR uses transaction-aware inode/lane reads, validates lane sequences and
base size, and rechecks decisions after its final row read. Reduction reads use
the committed lane replacement as their starting value. The new
`efs_meta_get_chunk_view_tx` resolves lane stamp/history replacements and owns
its per-part masks before RESOLVE, with bounded retries when decisions move.
The host's lane-authority preflight now reads the transaction-visible inode.
Regressions cover undecided/committed/aborted fences, replacement of existing
histories, commit during a chunk-view or stat collect, partial resolution and
unreachable coordinators; metadata and transaction suites pass normally and
under ASan/UBSan, and the full deterministic simulator passes locally. This
completes the metadata reader prerequisite; GETCHUNKS/client integration,
production coordination and live-file reclamation remain open.

**D25 resize-coordinator checkpoint (Oct 6, local and uncommitted).**
The shared coordinator captures a coherent inode/lane plan, freezes the inode
first to prevent lane activation and append reservation, prepares every active
lane, and persists one COMMIT before resolution. Failed or ambiguous preparation
requires durable ABORT before releasing holds; an ambiguous COMMIT is never
followed by ABORT. The internal server adapter uses the existing Raft transaction
appliers and recovery finisher. A distinct 74-byte resize PREPARE preserves the
old 65-byte fence subtype. Extensions retain epoch/history; shrink appends the
same global epoch to all authorities. First publication initializes a new lane
at its captured epoch. A lagging lane may skip earlier epochs only after a
first-row existence probe proves it has no chunk rows; populated lanes fail
closed. Closed append cursors retire atomically with inode/history intents;
open reservations return BUSY.
Tests cover all 65 authorities, partial preparation/resolution, lost decision
replies, applied-state restart recovery, stale snapshots, append exclusion,
empty-lane initialization, populated-lane epoch gaps, full history and atomic
three-intent storage failure. Full metadata and transaction suites pass normally
and under ASan/UBSan; the deterministic simulator passes locally. Strict server
syntax checks pass using a declaration-only Linux eventfd shim on macOS;
At that checkpoint Linux runtime/build and cluster gates remained pending;
the later GETCHUNKS checkpoint below records the completed isolated build.

**D25 GETCHUNKS/read-cache checkpoint (Oct 6, local and uncommitted).**
GETCHUNKS now reads committed transaction-aware inode and chunk views and
returns self-contained per-part masks/revisions with captured object identities.
Clients validate the reply before adoption, skip fully masked fragment GETs,
zero the fenced base tail before overlaying clipped spans, and refresh once
when captured fragments have disappeared. A failed refresh is an error.
Decoded cache keys include every mask/epoch and revision; a cache fill retains
the view it fetched rather than a newer map's identity. Demand and zero-copy
reads refresh maps before trusting cached bytes; partially fenced bases cannot
be served whole from the write cache. This adds metadata RPCs to cache hits;
performance needs measurement before claiming a throughput result.

Normal and ASan/UBSan captured-read/cache-fill/wire regressions pass. The full
Linux metadata, transaction and staging suites pass; strict changed-file syntax
checks and an isolated complete FUSE client build pass. The staging test covers
view retention across compaction and clearing on local generation replacement.
The wire record changed: rebuild and roll matching server/client binaries
before live gates; an installed replacement does not update an existing mount.

**W43/D25 is still open: production logical truncation is not enabled.**
SETATTR SIZE still uses the existing path. Next implement writer epochs and
masked merge/publication, including concurrent unpublished local bytes, then
bounded live-file sweep scheduling, durable progress and safe history
retirement. These must pass before connecting the logical resize coordinator.
The read-view phase above does not activate that coordinator.
Linux cold-read, concurrency, restart, history-full and apply-duration gates
remain pending. Changes are uncommitted; no cluster rollout. The separate unlink
sweep remains in place, and W54 still needs its post-GC cold-read cluster gate.

**D25 publication checkpoint (Oct 6, local and uncommitted).** Byte-backed
folds now retain the captured lane authority epoch separately from the mask
revision (which can include newer object epochs). That epoch follows the PUT
identity and its REPORT fallback; an older PUT cannot replace a newer
snapshot's identity. Before registration/proposal, the server rejects unknown
publication flags and captured epochs that differ from its inode epoch. The
apply request retains the captured epoch so the existing apply-side check also
covers a fence racing after that read. Cache keys include the authority epoch,
and wire validation rejects authority epochs beyond the view revision.

This closes the captured-fold re-stamping gap; it does **not** finish writer
integration. Metadata-only/unconditional publications retain the legacy path;
per-application dirty-range epochs, fence-aware merge of unpublished local
bytes, and the live-file sweep/progress/history-retirement work remain open.
Production logical truncation remains disabled. Fold/read/wire/pressure tests,
Linux metadata/transaction/staging tests and the complete FUSE build pass.
Matching server/client rebuilds remain required; no live rollout was performed.

**D27 / 0a checkpoint (Oct 6, foundation only, local and uncommitted).**
`include/efs/wb_recovery.h` starts the decided recovery policy with caller-locked
per-inode error state and per-record completed-cycle tracking. Tests cover
sticky errors even for reopened descriptions while unresolved, partial
resolution of two records, unseen historical errors once per description after
resolution, successful recovering sync, and fresh descriptions after recovery.
The stall tracker compares stable application operation identity plus server
generation, epoch and span count over eight completed cycles; movement resets
it, including a 1000-cycle changing-generation case. Standalone STALE replies
and failed fetches must never feed this tracker. Normal and ASan/UBSan tests
pass (`make test-wb-recovery`).

These helpers are **not wired into FUSE yet**. Next connect stable application
mutation identity and completed replay/publication accounting, retained
per-inode state and open-description cursors, recovery deadlines, write
admission and pinned dirty records, then controlled stop and the WITHHOLD fault
hook. No sticky-error, stall-detection or safe-stop runtime guarantee is claimed
by this checkpoint. 0a's historical inode investigation and live recovery /
contention gates remain pending; D28 salvage remains undecided.

**D27 runtime checkpoint (Oct 6, local and uncommitted; supersedes the
foundation-only checkpoint above).** The client now tracks stable application
mutation identity separately from PUT snapshot sequence. A successful rebase
stages an observation; only an attempted REPORT followed by a successful
authoritative repull proving that exact snapshot uncommitted completes the
cycle. Failed pulls propagate errors without classifying stale local maps as
committed. Verified residuals rebase even when the server state stayed still;
this changes the earlier resend optimization and needs live contention cost
measurement. Eight unchanged completed cycles mark the record stalled; server
movement resets the count, and contention diagnostics are emitted every 256
completed cycles.

Stalled records retain bytes and inode pins, reject new writes on that inode,
and survive ordinary cache-drop paths. Every FUSE file open now owns an error
cursor shared by its duplicated descriptors; separate opens retain separate
cursors. Sync/flush observations cannot clear an unresolved inode error.
Matching REPORT acknowledgment resolves only that record. Full-overwrite
bodies also remain within the hard allocator bound until acknowledgment,
including successful PUTs, so full-image STALE recovery has local bytes.
They are released after the matching commit; pressure may now force earlier
REPORT drains. Unrelated inode admission still uses the common hard bound.

A 30-second sync deadline begins before queued-write drain and append
serialization, with inheritance into replay and pooled PUT RPCs. **The strict
whole-call timing gate remains open:** accepted PUT jobs still own caller
completion state, and queue/lock cancellation must not free that state early.
Do not claim a hard 30-second fsync completion guarantee yet.

Controlled stop now uses a private owner-only Unix socket and gates complete
mutation-handler lifetimes before attempting a 60-second drain while mounted.
A timeout refuses detach and leaves accepted recovery work alive; retry waits
for that worker rather than reusing its state. Failed ordinary unmount resumes
mutations. `scripts/client.sh stop --force-discard /mnt/efs` explicitly logs
pending data ranges and permits lazy detach. Normal stop fails closed for old
clients without the channel. Explicit force on an old/dead client reports that
per-record diagnostics are unavailable. External unmount, SIGKILL and node
loss remain outside this contract; D28 salvage is unchanged and undecided.

WITHHOLD exists only in `EFS_FAULTS=1` builds and omits a selected record before
RPC serialization. The retained batch reconciles transmitted records through
authoritative repull. `OFF` in `/tmp/efs/fault` disables the startup target and
allows recovery without restart. Local tests cover completed-cycle authority,
partial acknowledgment, byte retention, descriptor errors, 1000 changing
server states, omission/disable, stop refusal, live-worker timeout ownership,
mutation quiescence and explicit detach. Normal and sanitizer gates pass;
Linux client builds pass. Live gates, the historical 0a inode investigation,
strict sync timing, coverage of legacy non-cache PUT fallbacks, and D25 dirty-range epochs / masked unpublished merges /
sweep scheduling / durable progress / history retirement remain open.
No deployment, workload rerun, mount change or commit was performed here.

**Where the project is (Oct 2 2026).** [architecture.md §10](../how-it-works/architecture.md)
steps 0–12 are landed and gated: simulator, KV, Raft, cross-shard txns,
sessions, directory spread, delete-2PC, FUSE A–D. The Raft+KV engine is
the only metadata engine (Step 11, Sep 11). There is no next §10 step.
What remains is the work queue in [§1a](#1a-the-work-queue): measured
gaps, in order. The live cluster state (build, leaders, recorders) is the
cluster fact in `.cursor/rules/efs-project-state.mdc`, not this page.

**How to pick work.** Finish [§1b](in-flight.md)
first. Then take the lowest-numbered open item in §1a; correctness before
performance. Each item names what to change, how to measure it, what
proves it, and what is forbidden. If an item needs a decision the spec
does not contain, **stop and ask** (§4). Decisions already taken are the
rows marked **decided** in §1a "Decisions"; implement them, do not
re-ask. **Oct 2 19:05Z: two open correctness rows go first — W54 (row
0i, a fold's GC deletes the live base → read EIO, data loss) and row 0j
(the client's 50 ms lookup memo returns pre-mutation stats, posix
164/201).** Open asks as of Oct 2 13:45Z: **D28** (who owns acknowledged
bytes across client death), **D29** (a REPORT receipt for "committed,
apply pending"; its live symptom did not appear untraced — P0.2),
**D30** (the remedy for compaction-induced apply lag), **W53**
(keep or revert W41 given the create/close-storm p99 37 → 100–120 ms
under four concurrent 8 GiB writers), **W52**'s fix shape (serial
reservation resolves make an O_APPEND REPORT exceed 30 s). Open
**investigations** — evidence gathering, not approved implementations:
**W48** (four dd streams ended early), **W53**. **W50** and **W51**
closed as investigations (14:57Z / 14:38Z). D27 was
corrected 04:20Z and is to be implemented as its row now reads. The
performance plan's P0, P1, P2.1 and P2.4 are done (§1b); **P2.2 D26**
is in tree and gated on the dev cluster (Oct 4: per-anchor pending-GC
watermark + the cursor; an idle leader runs no frag scan at all).
Everything else in the Oct 2 plan is decided, asked, closed or
deferred — see that table.

**What this page is not.** Approvals and decisions recorded here are
document claims about what the user said on the date given; they are
not authorization to mutate the cluster now. The long-form item texts
(sources, steps, forbidden lists for W6–W25) are in
[work-items.md](../backlog/work-items.md); superseded handoffs, closed items and
the old ordering tables are in project-history.md — cite its anchors:
[handoff archive Oct 1 18:00–22:00Z](../archive/project-history.md#ph-start-here-handoff-archive-oct-1-2026-1800z--2200z--moved-oc-8297ba),
[handoff archive Sep 28 – Oct 1](../archive/project-history.md#ph-start-here-handoff-archive-sep-28--oct-1-2026-1400z--moved-o-4e37f3),
[closed items moved Oct 2](../archive/project-history.md#ph-start-here-closed-items--moved-oct-2-2026-ea8027),
[closed items moved Oct 1](../archive/project-history.md#ph-start-here-closed-items--full-text-w1w5-w7-w11-w13-moved-oct-7d6df9).

**Before touching the cluster** run `tests/preflight.sh` (the deploy
rule's pre-flight as one command). Stop/start is
`tests/cluster.sh stop|start|restart [--clients] [--perf] [--strace]`;
a fresh table after a wipe is `cluster.sh start --fresh`. Runbooks for
the open measurement items are in [runbooks.md](../operations/runbooks.md)
(`tests/measure/*.sh`). **The one live cluster is port 19810** on
fcstor003–006 (`/data1/01–06/efs`, `--quota 36T --direct-io`, RDMA),
clients fcstor003–015 at `/tmp/efs-mount`, the user's own client on
fstor007 via `scripts/client.sh`. 19820 is retired. Do not
`wipe_cluster.sh`, `pkill -x efsd`, or `raft-mkfs` without being asked.

### 1a. The work queue

Rules for this queue: **take the lowest-numbered open item.** Do not start a
later item to avoid a harder earlier one — the order encodes a dependency and
a severity: **correctness items come before performance items**, because a
parallel filesystem that returns wrong bytes has no throughput number worth
reporting. Do not batch two items into one change. Every item ends with
`Forbidden`, which is binding.

Oct 6 triage: the user explicitly prioritizes **W59** because false ENOSPC
blocks the suite. After its rebuilt-client sustained-write and posix gates,
**W56** (root directory rename ghost) and **W57** (both rename parents' attrs)
have specific client paths and existing failing gates, making them the next
bounded implementation candidates. **W58** needs an opid/retry verdict trace;
**W55** needs a missing-fragment publication reproducer before choosing a fix.
The **W36 20/20 rename-vs-unlink gate** remains a release prerequisite for the
existing local correctness fix; none of these items is closed by this triage.

| # | item | class | status | home |
| --- | --- | --- | --- | --- |
| 0i | **W54** · a fold's GC deletes the live base → read EIO, data loss | correctness | IN TREE Oct 5, uncommitted — apply alias filters + regression; full metadata suite and ASan/UBSan pass locally; roll + cold cluster gate pending | [full text](../backlog/work-items.md#w54--a-folds-gc-deletes-the-live-base-queue-row-0i) |
| 0j | the client's 50 ms lookup memo returns pre-mutation stats (posix 164/201) | correctness | IN TREE Oct 5, uncommitted — guarded memo invalidation + mutation serial; local regressions and ASan/UBSan pass; Linux posix + Spark du gates pending | [full text](../backlog/work-items.md#0j--the-clients-50-ms-lookup-memo-returns-pre-mutation-stats-queue-row-0j) |
| 0k | **W55** · span committed to raft, fragment PUTs never landed → read EIO, data loss | correctness | open — found Oct 5 on the xorinox test cluster (write via the nfsd re-export from a macOS client); trigger not isolated | [full text](../backlog/work-items.md#w55--span-committed-to-raft-fragment-puts-never-landed--read-eio-data-loss-queue-row-0k) |
| 0l | **W56** · root-level rename leaves a ghost name in the renaming client's local lookup | correctness | open — posix gate in tree Oct 5 (`root_rename_dir_old_name_gone`, fails on the Oct 5 build); narrowed to **directory** renames with a root parent; fix in `metadata.c` rename local-apply (tab order of `efs_export_lookup`) | [full text](../backlog/work-items.md#w56--root-level-rename-leaves-a-ghost-name-in-the-renaming-clients-local-lookup-queue-row-0l) |
| mem1 | sparse dirty writes bypass reclaim; cache admission has no hard bound | correctness / resource exhaustion | in tree — Oct 5 working-tree implementation; local memory gates PASS; Linux load/cold-read/failure gates owed | [evidence, fix shape and gates](fuse-memory.md#sparse-dirty-writes-bypass-reclaim-and-cache-admission-has-no-hard-bound) |
| mem2 | read/readdir reply buffers leak when FUSE workers exit | resource lifetime | in tree — Oct 5 working-tree cleanup; 128 retired-worker and allocation/TLS-failure tests PASS; Linux FUSE worker gate owed | [evidence, fix shape and gates](fuse-memory.md#readreaddir-reply-buffers-leak-when-fuse-workers-exit) |
| 0m | parent dir mtime/ctime bump on entry create/unlink/rename/link (POSIX, §7.4) — ruled a bug if missing (user, Oct 6) | correctness | gate Oct 6 (nuc bare-metal cluster): posix `dir_times_*` 6/7 — create/unlink/mkdir/rmdir/link + same-dir rename all bump same-client; the cross-dir rename dst-parent failure is **W57**; posix2 `peer_dir_mtime_bump_visible` owed | [full text](../backlog/work-items.md#0m--parent-directory-mtimectime-must-bump-on-entry-createunlinkrenamelink-queue-row-0m) |
| 0n | **W57** · cross-directory rename never refreshes the dst parent's attrs on the renaming client | correctness | open — found Oct 6 on the nuc bare-metal cluster; posix gate in tree (`dir_times_rename` cross-dir phases, fail on the Oct 6 build); server verified correct via a second mount (ns-resolution bumps); a sibling layout also stales the SRC parent | [full text](../backlog/work-items.md#w57--cross-directory-rename-never-refreshes-the-dst-parents-attrs-on-the-renaming-client-queue-row-0n) |
| 0o | **W58** · open(O_EXCL) create answered EEXIST for a name the same client's own create just landed | correctness | analyzed Oct 6 (xorinox, build `3d3f17c2-dirty`): the file exists (created 05:17:31.319Z, size 0), the retry was answered BUSY (rc=-13, 05:17:31.733Z), no server logged EEXIST; suspect the retry path — an opid replay must return the recorded verdict (I16), not EEXIST; BUSY on unique-name creates is new with the dirty D25 intent probes | [full text](../backlog/work-items.md#w58--openo_excl-create-answered-eexist-for-a-name-the-same-clients-own-create-just-landed-queue-row-0o) |
| 0p | **W59** · write(2) via FUSE fails ENOSPC with 156 GiB free — client cache-admission mapped to ENOSPC; the 8 MiB metadata budget never drains | correctness | IN TREE Oct 6, uncommitted — metadata diagnostics + protected published-entry reclaim; local admission returns EAGAIN/ENOMEM; 8 MiB cap retained; metadata saturation + admitted-writer/drain reservation regressions and ASan/UBSan PASS; follow-up 251 MiB ENOMEM reproduced locally and reservation fix added; remount + sustained-write and posix jobs=1 gates owed | [full text](../backlog/work-items.md#w59--write2-via-fuse-fails-enospc-with-156-gib-free--client-cache-admission-mapped-to-enospc-the-8-mib-metadata-budget-never-drains-queue-row-0p) |
| 0e | **W38** · ior-hard fold tombstone without the span's bytes | correctness | IN TREE Oct 5, uncommitted — replay preserves live spans; folds require byte observations; deterministic regression + ASan/UBSan pass; traced IOR-hard + cold hardscan gate pending | [full text](../backlog/work-items.md#w38--ior-hard-fold-tombstone-without-the-spans-bytes-queue-row-0e) |
| 0c | **W36** · rename-vs-unlink of one source both succeed, dangling dentry | correctness | in tree — **RECURRED Oct 6 00:41Z on the xorinox cluster** (build `3d3f17c2-dirty`, which contains the fix): dangling `b` in `posix-2c/peer_rename_vs_unlink_src` — `raft-readdir` lists it, `raft-lookup` returns ino=0; second pre-PREPARE race reproduced and exact-source fix now local/uncommitted; D25 causation unproven; Linux build and 20/20 gate still owed | [full text](../backlog/work-items.md#w36--rename-vs-unlink-of-one-source-both-succeed-dangling-dentry-queue-row-0c) |
| 2a | **W42** · `df` / `efs-mgmt status` report the 3-node capacity model on any node count | correctness | in tree — verify on 19810; one-QUOTA-member PUT question open | [full text](../backlog/work-items.md#w42--df--efs-mgmt-status-report-the-3-node-capacity-model-on-any-node-count-queue-row-2a) |
| 0b | **W27** · REPORT identity from the staging table | correctness | rerun on the current client; close if `putid miss` is 0 | [full text](../backlog/work-items.md#w27--report-identity-from-the-staging-table-queue-row-0b) |
| 0g | **W43** · big truncate is a silent no-op past 32 chunks per lane; step a = **D25** (decided) | correctness | D25 metadata primitives, committed readers and internal resize coordinator in tree, uncommitted; public read-cache/writer/sweep integration open | [full text](../backlog/work-items.md#w43--truncateo_trunc-of-a-file-with--32-chunks-in-a-lane-is-a-silent-no-op-queue-row-0g) · [D25](decisions.md) |
| 0a | STALE replay that never converges; remedy = **D27** (decided) | correctness | implement D27 as its row reads, with 0a (a)–(c) | [full text](../backlog/work-items.md#0a--stale-replay-that-never-converges-queue-row-0a) · [D27](decisions.md) |
| 0h | **W44** · the leader's GC frag pass scans the whole prefix every 1.2 s; remedy = **D26** | performance | D26 in tree + gated (dev cluster, Oct 4): watermark gates the scan, cursor bounds the pass; idle leaders logged no `gc-pass` line for 10 min; a 5120-record `rm` drained at ~514 records/pass | [full text](../backlog/work-items.md#w44--the-group-leaders-gc-frag-pass-scans-the-whole-prefix-every-12-s-queue-row-0h) · [D26](decisions.md) |
| 12 | **W48** · four of the 16 dd streams ended early | investigation | investigate — evidence only | [full text](../backlog/work-items.md#w48--four-of-the-16-dd-streams-ended-early--investigate-plan-row-12) |
| 15 | **W52** · a REPORT after thousands of O_APPEND writes answers after > 30 s | correctness | decided Oct 5 (user): shape A — one batched proposal for all caught-up reservations before the reply; gate = `append_gate` | [full text](../backlog/work-items.md#w52--a-report-after-thousands-of-o_append-writes-answers-after--30-s-plan-row-15) |
| 16 | **W53** · W41's create/close-storm p99 regression — keep or revert | investigation | investigate — evidence only, then the user decides | [full text](../backlog/work-items.md#w53--w41s-createclose-storm-tail-under-concurrent-big-writers--investigate-plan-row-16) |
| E | **D17** · `st_blocks` = 0 for files this client did not write | performance | in tree + gated (dev cluster, Oct 4): lane-stamp present count, summed at getattr, client takes max with its local table; `du` on a non-writing client = size/512 | [D17](decisions.md) |
| P2.2 | **D26** · the GC pass | performance | in tree + gated (dev cluster, Oct 4): per-anchor pending-GC watermark maintained in the apply, derived once per recovery/import, `zero_if` clamp on a drained pass; `test_gc_watermark`; live: 514 records/pass drain, 10 idle min with no `gc-pass` line | [full text](../backlog/work-items.md#p22--d26--the-gc-pass-performance-plan-row) · [D26](decisions.md) |
| P2.3 | **W23** · stalled-compactor test | performance | test + hook in tree; measured Oct 5Z (dev cluster): no stall-specific effect to 4.35 GiB (n_l0 never left 0 — the stall never bit; rss-2x stop = small-VM calibration artifact); refinements named in SUMMARY | [full text](../backlog/work-items.md#p23--w23--the-stalled-compactor-test-performance-plan-row) · [run](../../results/measure/20261005-040810-w23-stalled-compactor/SUMMARY.txt) |
| P3 | `efsd --bench`, then `efs-fuse --bench` | performance | tools in tree + gated (dev cluster, Oct 5); first numbers measured on efs1 (write ≈82 % of the 1-disk fio ceiling at QD16; meta = the 36 ms fsync wall, batch ×32; client cpu 2.8 ≫ put 0.26 ≈ write GiB/s). Owed: the 6-NVMe fcstor run (named host) and the two-host client ladder | [server plan](../backlog/work-items.md#single-node-storage-bench-efsd---bench--asked-oct-2-2026-user-queue-position-after-w41--d23--d17--d26-in-plan-after-the-oct-1-2200z-review-its-number-decides-the-fragment-layout-w40-and-zero-copy-receive) · [client plan](../backlog/work-items.md#client-bench-efs-fuse---bench--asked-oct-2-2026-user-after-efsd---bench) · [run](../../results/measure/20261005-045140-p3-benches/SUMMARY.txt) |
| P4.1–P4.4 | fragment on-disk layout (**wipe**), W40 FUSE write copy, RDMA zero-copy receive, 9-client scaling | performance | deferred until P3's numbers | [full text](../backlog/work-items.md#p4--deferred-until-p3s-numbers-long-one-is-a-wipe) |
| meta-scale | metadata leadership distribution and topology-independent routing | scalability | open — Oct 5 implementation gap recorded; correctness work first, then baseline measurement and staged multi-Raft implementation | [evidence, phases and gates](metadata-scaling.md) |
| — | **D28** (who owns acknowledged bytes across client death) · **D29** (a REPORT receipt for "committed, apply pending") · **D30** (the remedy for compaction-induced apply lag) | ask | ask — not code until the user decides | [decisions.md](decisions.md) |
| — | **io-stats** · always-on per-op-class data-plane counters (`iostats:` log line + `efs-mgmt io-stats`) | observability | landed (dev cluster, Oct 4) | [note](../backlog/io-stats.md) |
| — | **version reporting** · `--version` on all five binaries, startup log lines, `EFS_MSG_VERSION` op, `efs-mgmt version`, `status` Versions line | observability | landed (dev cluster, Oct 5) | [code](../../include/efs/version.h) |

**Order of work from here (one order, correctness first — Oct 2
04:20Z):** (1) **D25** (logical truncation + background reclamation) completes W43 (`truncate_big.sh` exit 3 → 0 with t1–t9, `apply_max` flat);
(2) **D27** with 0a (a)–(c) and its gates; (3) the correctness gates
still owed — W36 20/20, W38 (row 4), W27 rerun (row 6), the W42
one-QUOTA-member PUT question; (4) only then the measurements — W44 a
idle-hour reading, W46/W47 counts (rows 9/10), the untraced 16× dd
re-measurement (row 11); (5) D23, W41, D17, D26 (after step 4's W44
reading); (6) `efsd --bench`, then `efs-fuse --bench`; (7) the open
asks go to the user — D28 with 0a (a)'s answer and D27's gate result,
D29 with W41, D30 with W51's table. The investigations W48 / W50 / W51
(rows 12–14) are quick evidence tasks that may run at any point the
cluster is up; they are not approvals to implement anything. §1b
"Next" is this list and nothing else.

**Dependencies, in one line.** P0 needs a cluster roll (no `--strace`).
P1.1 and P1.2 are independent of each other and of the servers; P1.2
wants P1.3 decided to realise its full effect but does not need it.
P2.2 waits for P2.1; P2.5 waits for P2.4. P3 needs no cluster. P4 needs
P3. Nothing here needs a wipe except P4.1.

Hardware ceilings and honest baselines now live in [performance.md](../how-it-works/performance.md#baselines-and-ceilings-current), including the normative sentence "if a benchmark stops at a mutex … that is by definition an EFS bug" and today's "write path = 9 % of client ceiling" state.


## Review and commit checkpoint — Oct 6 2026

The reviewed implementation is committed in `bb18e40a` (metadata fence views,
transaction identities and alias-safe GC), `033a842a` (FUSE memory admission,
publication recovery, worker cleanup and controlled stop), and `3d9bb6b2`
(POSIX acceptance tests). Earlier dated “uncommitted” checkpoints describe
the state at that time; these commits supersede that working-tree status.
GETCHUNKS fan-out now inherits the caller recovery deadline. Local memory
and runtime gates pass, including ASan/UBSan; an isolated Linux build and full
`make test` pass.

D25 and D27 remain open: public logical truncate activation, writer/sweep
integration, strict whole-call deadlines, and live fault/RSS acceptance still
need their recorded gates. W36 needs the live rename-versus-unlink 20/20 gate;
no existing dangling dentry was removed during this review. No live deployment
or workload rerun was performed.


## D25 partial-writer merge checkpoint — Oct 6 2026

Implemented in `8727f682`. The generation-only merge shortcut has been removed: a fence may change the
valid byte ranges of the same object. Every partial snapshot now obtains a
fresh GETCHUNKS view and masked published image before overlaying its owned
ranges. The exact fetched base generation, span observation and captured
authority epoch travel with the PUT/REPORT. An authoritative absence never
reads this client's staged, unreported object as a committed base. RPC/fetch
failures leave accepted local bytes dirty and pinned, with balanced publication
windows and the original error code. Whole-chunk unconditional overwrites keep
their existing path.

The regression executes both production flush functions, reproduces the old
fenced-tail resurrection with an unchanged generation, and checks empty-row
CAS, BUSY/NOMEM retention and full-overwrite behavior. Normal and ASan/UBSan
gates pass; strict Linux syntax, an isolated complete build and full
`make test` pass. Partial flushes now do additional metadata/fragment reads; live
performance is unmeasured. This is the published-base portion of writer
integration: local dirty-range epoch capture, clipping pre-fence unpublished
bytes, unconditional publication epochs, and live-file sweep scheduling and
history retirement remain open. Public logical truncation stays disabled.
