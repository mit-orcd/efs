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
| 0l | **W56** · root-level rename leaves a ghost name in the renaming client's local lookup | correctness | fixed in `f8fef814` — exact old directory-name cache eviction across parent/hash tabs; NUC full POSIX jobs=4/jobs=1 and two-client suites PASS Oct 6; xorinox roll still owed | [full text](../backlog/work-items.md#w56--root-level-rename-leaves-a-ghost-name-in-the-renaming-clients-local-lookup-queue-row-0l) |
| mem1 | sparse dirty writes bypass reclaim; cache admission has no hard bound | correctness / resource exhaustion | in tree — Oct 5 working-tree implementation; local memory gates PASS; Linux load/cold-read/failure gates owed | [evidence, fix shape and gates](fuse-memory.md#sparse-dirty-writes-bypass-reclaim-and-cache-admission-has-no-hard-bound) |
| mem2 | read/readdir reply buffers leak when FUSE workers exit | resource lifetime | in tree — Oct 5 working-tree cleanup; 128 retired-worker and allocation/TLS-failure tests PASS; Linux FUSE worker gate owed | [evidence, fix shape and gates](fuse-memory.md#readreaddir-reply-buffers-leak-when-fuse-workers-exit) |
| 0m | parent dir mtime/ctime bump on entry create/unlink/rename/link (POSIX, §7.4) — ruled a bug if missing (user, Oct 6) | correctness | gate Oct 6 (nuc bare-metal cluster): posix `dir_times_*` 6/7 — create/unlink/mkdir/rmdir/link + same-dir rename all bump same-client; the cross-dir rename dst-parent failure is **W57**; posix2 `peer_dir_mtime_bump_visible` owed | [full text](../backlog/work-items.md#0m--parent-directory-mtimectime-must-bump-on-entry-createunlinkrenamelink-queue-row-0m) |
| 0n | **W57** · cross-directory rename never refreshes the dst parent's attrs on the renaming client | correctness | fixed in `f8fef814` — refresh both parent attrs after rename and adopt authoritative directory attrs; NUC full POSIX jobs=4/jobs=1 and two-client suites PASS Oct 6 | [full text](../backlog/work-items.md#w57--cross-directory-rename-never-refreshes-the-dst-parents-attrs-on-the-renaming-client-queue-row-0n) |
| 0o | **W58** · open(O_EXCL) create answered EEXIST for a name the same client's own create just landed | correctness | analyzed Oct 6 (xorinox, build `3d3f17c2-dirty`): the file exists (created 05:17:31.319Z, size 0), the retry was answered BUSY (rc=-13, 05:17:31.733Z), no server logged EEXIST; suspect the retry path — an opid replay must return the recorded verdict (I16), not EEXIST; BUSY on unique-name creates is new with the dirty D25 intent probes | [full text](../backlog/work-items.md#w58--openo_excl-create-answered-eexist-for-a-name-the-same-clients-own-create-just-landed-queue-row-0o) |
| 0p | **W59** · write(2) via FUSE fails ENOSPC with 156 GiB free — client cache-admission mapped to ENOSPC; the 8 MiB metadata budget never drains | correctness | IN TREE Oct 6, uncommitted — metadata diagnostics + protected published-entry reclaim; local admission returns EAGAIN/ENOMEM; 8 MiB cap retained; metadata saturation + admitted-writer/drain reservation regressions and ASan/UBSan PASS; follow-up 251 MiB ENOMEM reproduced locally and reservation fix added; remount + sustained-write and posix jobs=1 gates owed | [full text](../backlog/work-items.md#w59--write2-via-fuse-fails-enospc-with-156-gib-free--client-cache-admission-mapped-to-enospc-the-8-mib-metadata-budget-never-drains-queue-row-0p) |
| 0e | **W38** · ior-hard fold tombstone without the span's bytes | correctness | IN TREE Oct 5, uncommitted — replay preserves live spans; folds require byte observations; deterministic regression + ASan/UBSan pass; traced IOR-hard + cold hardscan gate pending | [full text](../backlog/work-items.md#w38--ior-hard-fold-tombstone-without-the-spans-bytes-queue-row-0e) |
| 0c | **W36** · rename-vs-unlink of one source both succeed, dangling dentry | correctness | exact-source PREP guards committed earlier; `75b06624` also returns simple UNLINK apply verdict; NUC rename-vs-unlink 20/20 and full two-client 64/64 PASS again on 79983128 Oct 6; xorinox b4a75492 current-build race gate 20/20 PASS; historical dangling b repaired by guarded Raft unlink; du/dua clean | [full text](../backlog/work-items.md#w36--rename-vs-unlink-of-one-source-both-succeed-dangling-dentry-queue-row-0c) |
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
| P3 | `efs-bench --bench data/meta`, then `efs-fuse --bench` | performance | tools in tree + gated (dev cluster, Oct 5); Oct 6: local CLI moved to `efs-bench`, raw parallel read/write isolation (`io`), identical I/O with BLAKE3 (`io-blake3`), CPU-only BLAKE3 and `efs-bench.sh` baseline/perf/optional separate strace harness added ([usage](../how-it-works/performance.md#benchmark-profiling-harness)); first numbers measured on efs1 (write ≈82 % of the 1-disk fio ceiling at QD16; meta = the 36 ms fsync wall, batch ×32; client cpu 2.8 ≫ put 0.26 ≈ write GiB/s). Owed: the 6-NVMe fcstor run (named host) and the two-host client ladder | [server plan](../backlog/work-items.md#single-node-storage-bench-efsd---bench--asked-oct-2-2026-user-queue-position-after-w41--d23--d17--d26-in-plan-after-the-oct-1-2200z-review-its-number-decides-the-fragment-layout-w40-and-zero-copy-receive) · [client plan](../backlog/work-items.md#client-bench-efs-fuse---bench--asked-oct-2-2026-user-after-efsd---bench) · [run](../../results/measure/20261005-045140-p3-benches/SUMMARY.txt) |
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
reading); (6) `efs-bench --bench data/meta`, then `efs-fuse --bench`; (7) the open
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


## D25 dirty-range foundation checkpoint — Oct 6 2026

Committed in `6221d22f`: `include/efs/dirty_ranges.h` implements bounded
epoch-labelled ownership for
application bytes. Overlapping writes replace only their covered bytes and
split older ranges; adjacent ranges coalesce only at the same epoch. At most
32 ranges are owned. Overflow returns BUSY without changing the state; the
caller must drain before accepting/copying that write. There is no collapse
to an unconditional whole-chunk overwrite. Snapshot copies retain original
ages and a mutation identity. Acknowledgement clears only the exact original
snapshot; concurrent rewrites remain owned. The caller must also validate the
PUT/REPORT identity before acknowledging.

Clipping uses the shared fence validity rule and an authoritative history
completeness floor. Snapshots older than the retained complete history return
STALE; an empty/retired history never silently revives old bytes. Surviving
local ranges overlay an already-masked published image, preserving peer bytes
under discarded dirty suffixes. Failed validation leaves outputs untouched.

`make test-dirty-ranges` passes normally and under ASan/UBSan. The independent
50,000-step byte model checks ownership ages, overlapping writes, repeated
shrinks/extensions and clipped overlay. Deterministic cases cover full
ownership splits, range exhaustion, mutation overflow, stale acknowledgements
and incomplete-history rejection. The target is included in `make test`; the full isolated Linux build/test
suite and all five documentation checks pass.

**This is a tested writer primitive, not activated FUSE epoch tracking.** The
current GETCHUNKS view exposes masks for published parts, not a complete
history for arbitrary local writer epochs; absent rows expose no authority
stamp. The next step must add an authoritative writer snapshot, including
FileID, authority epoch, complete history and its retirement floor, before
write admission can assign epochs. It must cover holes/new lanes and reject
retirement races. Then replace the existing dcache range union under its lock,
reserve any increased metadata budget, capture matching bytes/ranges for
flush/retry, and acknowledge only matching snapshots. Never use cached
inode/chunk epochs or assume a retired history is complete from epoch zero.
Logical truncation remains disabled.


## NUC deployment gate — Oct 6 2026, 16:26Z

The user authorized the driver workflow
`/Users/mike/git/devops/nuc-efs/deploy-and-restart.sh` for future code changes.
It stops, rsyncs the working tree, rebuilds server/client binaries on `nuc_efs`,
starts three loopback servers and runs POSIX smoke. `--full` adds two-client
visibility and prepare/clean-remount/verify durability gates. Results are under
`/data1/efs/logs`. These are three processes on one host, not three independent
failure domains. Its deployment builds binaries only; isolated `make test`
remains a separate prerequisite.

**This attempt did not deploy or rerun suites.** Clean-stop preflight on
`/data1/efs/mnt` refused after its drain: `stop-refused: unresolved writes
remain; mount retained` at 16:26:27.154Z. All three original server PIDs and
both original client PIDs remained running. Running binaries identify as
`v0.1.0-pre-alpha-22-g1b7ed050`, built 16:14:15Z. The wrapper's `stop.sh`
automatically force-discards after clean-stop refusal; that fallback was not
executed. Investigate retained writes before requesting explicit discard.

Existing results collected read-only, from earlier runs (not this attempt):

- `/data1/efs/logs/posix-20261006-122007.tsv`: 207 PASS, 9 FAIL, 1 SKIP.
  Failures: both large honest-truncate tests (EIO), root directory rename old
  name remains, rename destination directory mtime, unlink-open, disjoint
  concurrent writes, nlink-after-unlink-open, unlink/recreate with an open old
  file, and unlink/recreate new inode. Most report EIO; this does not establish
  a common root cause.
- `/data1/efs/logs/posix2c-20261006-121440.tsv`: 62 PASS, 2 FAIL.
  `peer_overlap_pwrite_chunk_straddle`: exclusive above-chunk range was not B.
  `peer_rename_vs_unlink_src`: rename and unlink both succeeded. W36's live
  gate therefore remains failed/open; no 20/20 claim is justified.
- `/data1/efs/logs/persist-prepare-20261006-121623.tsv`: 26 PASS. No matching
  verify result was found; prepare alone does not prove cold durability.

These results include the preceding partial-merge change but predate the
dirty-range primitive commits. That primitive is not connected to FUSE and
cannot explain or resolve this live failure. Do not claim the NUC acceptance
gate passed. Preserve current pending writes and test artifacts for diagnosis.

## NUC correctness fixes — Oct 6 2026, follow-up

The original NUC mounts and three servers were retained. The clean-stop
failure was diagnosed by reading the old client's cache without modifying it:
eight dirty chunk bodies initially, then twelve at capture time, reference
inodes that authoritative GETATTR says are deleted. The twelve bodies (1.5 MiB)
and a checksum manifest are preserved at
`/data1/efs/logs/retained-350696-20261006/`. This is diagnostic evidence, not a
claim that the deleted files have been recovered. The old binary still needs
an explicit recovery/discard decision before its mount can be replaced.

Six fixes are committed:

- `a07fbfb5`: CREATE takes the inode HOLD required for unlink-open semantics.
  Serialize first-open/last-close lease RPCs; flock flush no longer drops an
  inode lease while another descriptor exists. Allocation/HOLD failures unwind.
- `75b06624`: simple UNLINK returns its apply verdict, rather than reporting
  success merely because the Raft index settled. A losing rename/unlink race
  now reaches the client as a rejection.
- `f8fef814`: W56/W57 directory cache fixes. Forget only matching old-name
  dentry copies and refresh both rename parents from committed GETATTR; cached
  directory attrs are updated without overwriting regular-file dirty bytes.
- `1a12aa25`: sparse truncate-grow creates a new tail lane at the truncate
  epoch. Previously its chunk was epoch 1 while its lane stamp remained 0,
  making captured-epoch writes repeatedly STALE (`concurrent_writes_disjoint`).
- `c7a56201`: a dirty range overlapping a committed span uses a full CAS fold.
  The former suffix-only selection dropped the application's overlapping
  overwrite (`peer_overlap_pwrite_chunk_straddle`). Disjoint spans remain spans.
- `febc55e5`: clear phantom span-only REPORT marks when neither a PUT identity
  nor local dirty/stalled/pinned/unreported ownership remains. Both initial and
  STALE-rebuild paths preserve marks for actual local bytes. This addresses a
  W27-adjacent clean-stop loop; the nonzero staging-identity fallback is still
  a separate open audit item.

Validation ran on separate NUC loopback clusters (ports 18432–18434 and
19432–19434), without replacing the original cluster on 17432–17434. These
source snapshots have build ID `unknown` because they contain no Git metadata;
they contain the listed code changes. Linux `make test` passes. Both sparse-tail
and overlap-selection regressions fail against the preceding production source
and pass with their fixes; lease and span-selection ASan/UBSan checks pass.
Full POSIX jobs=4 and jobs=1 each return **216 PASS, 0 FAIL, 1 mmap SKIP**;
full two-client suite returns **64 PASS, 0 FAIL**. W36 and chunk-straddle gates
pass **20/20** each. Clean-unmount/remount durability has **26/26 prepare and
26/26 verify PASS**. Test teardown then exposed phantom span-only marks on
older peer clients; the final drain fix reran **64/64 PASS**, followed by clean
stop of both new clients without discard. Older diagnostic/test mounts with
pending work were retained; no force-stop was used. [Saved results](../../results/measure/20261006-nuc-correctness/SUMMARY.md).

This does not finish D25: logical resize remains internal, the epoch-owned
range primitive is not wired into FUSE, and complete writer/absent-chunk
authority is still pending. CREATE then HOLD is also still two RPCs: the lease
is guaranteed before returning the descriptor, but an atomic create-and-open
operation is needed to close the remote-unlink window between those RPCs.
The NUC gates do not replace the xorinox roll, IOR-hard, or fault/RSS gates.


## D25 inode writer-authority snapshot checkpoint — Oct 6 2026

`efs_meta_get_writer_view_tx` now captures FileID, inode authority epoch,
retained fence history and its completeness floor without requiring a chunk
row or an active lane. Inode/history double collection and transaction-decision
revalidation reject mixed applies; four unsuccessful collections return BUSY
without changing the caller's output. Committed, unresolved fence transactions
are visible; undecided ones retain the previous authority.

The completeness floor is derived conservatively from the contiguous retained
history suffix ending at the current authority epoch. A gap, missing newest
fence or entirely retired history cannot silently authorize epoch-zero dirty
bytes. Legacy physical truncation without history also raises the floor.
Snapshots carry their history by value, independent of later retirement.

Linux metadata regressions cover empty files, retired interior gaps, empty
history at a nonzero stamp, FileID mismatch, future history, concurrent fence
retry, bounded repeated races and committed/undecided transaction visibility.
The full Linux `make test` passes. This internal API changes no public FUSE
behavior and needs no cluster rollout yet.

**Next:** expose this inode snapshot through an authoritative wire operation,
validate/adopt its epoch on the publication lane (including absent lanes), then
connect bounded dirty ranges to FUSE admission and exact flush acknowledgements.
The inode snapshot alone is not a lane publication permit. Public logical
truncate, live sweep scheduling and history retirement remain outstanding.


## D25 writer snapshot RPC and lane validation checkpoint — Oct 6 2026

`INODE_WRITER_VIEW` (107/108) now exposes the bounded writer snapshot through
an inode-routed RPC. The host establishes ReadIndex authority on both inode
and requested publication-lane groups, forwarding when it cannot host both.
Generation zero discovers FileID because existing LOOKUP replies omit its
generation; subsequent requests can require the exact discovered generation.
Replies include chunk identity, authority epoch, complete-history floor and
retained history. Client validation rejects mismatched identities, malformed
histories and floors that claim completeness across retired gaps. Failed reads
leave the previous snapshot unchanged; redirect/BUSY retries are bounded.

`efs_meta_get_writer_chunk_view_tx` collects the lane stamp around the inode
snapshot and rechecks transaction decisions. An absent inactive lane may use
the captured inode epoch; first publication already initializes its stamp at
that epoch. An active missing lane or a present lane at a different epoch
returns BUSY. This read performs no adoption or mutation; lagging lanes still
need the existing coordinated resize/adoption path.

Linux `make test` passes. RPC extraction tests pass normally and under
ASan/UBSan, covering redirects, BUSY exhaustion, bad type/length/identity,
incomplete history, STALE and transport failure. Metadata tests cover absent
and lagging lanes, first-publication epoch initialization, stale publication,
and committed/undecided fence transactions before RESOLVE. On the isolated
NUC cluster, **216 live checks PASS** across all three nodes for holes, FileID
mismatch and published rows; malformed requests are rejected. Both test clients
cleanly stop without discard. [Saved results](../../results/measure/20261006-writer-rpc/SUMMARY.md).

**Next:** bind FileID/authority snapshots to bounded FUSE dirty-range admission,
retain matching bytes/ranges across flush/retry and acknowledge exact owned
snapshots. History-retirement races must retain accepted bytes and fail closed;
the RPC itself grants no lifetime guarantee against a later fence. Public
logical truncate and live sweep/retirement remain disabled/pending. This phase
has not changed production write admission or rolled the original NUC mounts.


## D25 FileID-bound writer planning checkpoint — Oct 6 2026

`include/efs/writer_ranges.h` now connects authoritative writer replies to the
bounded dirty-range primitive. Admission captures the exact FileID, chunk and
authority epoch, rejects identity changes/regressed authority and commits
ownership only on success. Capacity overflow returns BUSY without changing
ranges or promoting sparse bytes to an unconditional full-chunk overwrite.
The caller must reserve metadata and succeed at admission before copying bytes.

Publication planning requires a freshly materialized published base at the
same authority epoch as the writer snapshot, clips each locally owned range
against complete retained history, and keeps both original acknowledgement
identity and surviving byte ownership. Lost history returns STALE with the
previous plan and accepted ranges intact; it never recaptures old bytes as new.
Only the surviving ranges overlay peer data. Exact acknowledgement uses the
original snapshot, FileID and chunk; concurrent rewrites remain owned. The
caller must separately match the committed PUT/REPORT object and sequence.

Normal and ASan/UBSan tests cover mixed-age overwrites across successive fences,
peer bytes under discarded suffixes, missing history, mismatched base authority,
FileID/chunk changes, range exhaustion, failed initial admission, exact ACK and
fully fenced chunks. The target is included in the full Linux `make test`,
which passes with all five documentation checks.

**Not activated in FUSE yet.** The existing cache still unions ranges and its
old overflow fallback can promote sparse ownership to a full overwrite. The
next runtime change must replace that union under its lock, account for the
larger ownership record, capture matching bodies/ranges in both flush paths,
retain ownership across PUT/REPORT failure and publish only a matching plan.
History retirement also needs an explicit retained-byte recovery policy before
public logical truncate can be enabled. This checkpoint adds no runtime RPCs,
changes no mounts and does not claim the live D25 acceptance gates are complete.


## D25 five-round transport/ownership checkpoint — Oct 6 2026

Five implementation rounds follow the FileID-bound planner:

1. `7072d97b`: GETCHUNKS validates the entire fixed reply before exposing cache
   records: count/request bounds, inode identity, ordered distinct chunk indices,
   group bounds and per-part views. Wrong lengths and malformed replies fail closed.
2. `b6ca5f86`: GETCHUNKS requests may require an exact FileID generation;
   forwarded requests retain it. Replies carry FileID and inode authority epoch
   even for holes. `efs_client_rpc_getchunks_fileid` leaves outputs unchanged on
   error; the existing getter remains its discovery-mode wrapper.
3. `c093b021`: the planner checks the actual base reply's FileID, chunk geometry
   and both inode/lane epochs. A `max=1` result that skips a hole and returns a
   later chunk is never used as the requested base. The fifth round turns
   this proof of a hole into an explicit zero-image/zero-CAS plan instead of
   repeatedly retrying the same later row.
4. `972bbca9`: immutable publication tokens bind original ownership to PUT object
   generation and snapshot sequence. Failed/mismatched REPORT cannot acknowledge
   ownership; concurrent rewrites prevent an older snapshot clearing their bytes.
5. Budgeted writer sidecars allocate through the metadata hard bound. Pending
   ownership/publication prevents destruction. A matching committed older PUT
   retires its token while keeping concurrently rewritten ranges for another PUT.
   Committed publication also advances observed authority, preventing later
   admission from regressing to a pre-fence snapshot.

Linux full `make test` and all five documentation checks pass. Planner and
allocator/lifetime regressions pass under ASan/UBSan. An isolated NUC native
wire probe passes **432 checks** across all three nodes, including GETCHUNKS
holes, FileID mismatches and published rows. Single-client POSIX is **216 PASS,
0 FAIL, 1 mmap SKIP**; two-client POSIX is **64/64 PASS**. Both new clients
cleanly stop without discard. [Saved results](../../results/measure/20261006-writer-five-rounds/SUMMARY.md).
GETCHUNKS wire shapes changed; deploy matched server/client binaries together.
The original mounts and older retained diagnostic clients were unchanged.

**Still outstanding:** attach the budgeted ownership sidecar to dcache entries,
reserve its full metadata charge before copying writes, replace the range union,
and carry immutable plans/tokens through both flush paths and REPORT/retry.
One sidecar currently owns one pending publication; runtime integration must
serialize it or explicitly bound multiple in-flight plans. Incomplete history
must retain accepted bytes, never re-age or discard them. Public logical truncate
and history retirement remain disabled/pending; these five rounds do not
activate epoch-based FUSE writes or complete D25.

## D25 cache lifetime/admission checkpoint — Oct 6 2026

Another five rounds fix two active late-loader races (`2fd76ef1`, `fb88cb6f`),
protect optional writer state through cache lifetime (`fd44a8c0`), add a
budgeted admission-before-copy API (`c0f7efee`), and capture immutable matching
body/range snapshots. REPORT completion now checks typed publication tokens;
legacy clean/committed flags cannot release a body while typed ownership remains.
The race regressions fail against the previous implementations. Full Linux
build/tests, local normal/ASan/UBSan tests and the documentation gate pass.
Private NUC acceptance is 216 PASS/0 FAIL/1 mmap SKIP for single-client POSIX
and 64/64 PASS for two-client POSIX. Both fresh clients stopped without discard.
[Evidence and live acceptance](../../results/measure/20261006-cache-five-rounds/SUMMARY.md).

**Still outstanding:** sidecar allocation/admission is not activated in FUSE.
All write entry points and both flush paths must adopt the authoritative API,
retain matching snapshots, serialize pending publications and drain bounded
range exhaustion before accepting bytes. The legacy unlabelled range union
still runs and can collapse sparse ownership on overflow; this checkpoint does
not claim that peer-hole overwrite risk is fixed. Never re-age existing legacy
writes to manufacture admission authority. Logical truncate/history retirement
and D27 timing/fault/RSS gates remain open.

## D25 overwrite/binding checkpoint — Oct 6 2026

Five further rounds make full-overwrite bytes and range reset atomic
(`882d2dbd`), retain the cache-hit path without extra allocation (`0bbd33af`),
reject a second snapshot while REPORT is pending (`a229b30d`), and add clean
cache binding to expected FileID/authority (`fce4e629`). The binding API refuses
legacy dirty, pinned and unreported bytes; no accepted legacy bytes acquire a
new epoch. Write extent validation now rejects negative offsets, callback count
overflow, end overflow and chunk-index/exclusive-end overflow before copying.
Actual append reservations are checked before pins/invalidation/index casts.
[Tests and acceptance](../../results/measure/20261006-admission-five-rounds/SUMMARY.md).

Full Linux build/tests, local normal/ASan/UBSan regressions and the five
checks in the documentation gate pass. Private NUC acceptance is 216 PASS,
0 FAIL, 1 mmap SKIP plus 64/64 two-client PASS; both clients stop cleanly.
The full-overwrite regression fails against the previous production function.
The cache binding API is staged; authoritative FUSE admission and both flush paths still
need wiring before activation. The active legacy union's sparse-overflow risk,
logical truncate/history retirement and D27 fault/timing/RSS gates remain open.

## D25 token checks and admission-routing decision — Oct 6 2026

Three further rounds validate that publication ranges/ages remain subsets of
accepted ownership (`dccd8a57`), retain tokens on invalid REPORT completion
(`a5dfacd8`), and reject cache/publication geometry or future-mutation mismatches.
Linux build/tests, normal/ASan/UBSan regressions and the documentation gate pass.
[Evidence](../../results/measure/20261006-writer-token-checkpoint/SUMMARY.md).

The user accepted the [lane-local admission route](d25-admission-routing.md):
the existing writer RPC consults both inode and lane authorities, whereas §7.3
keeps ordinary writes on lane authority. Implement lane-local authoritative
views before activation; do not enable the temporary per-write inode RPC. No authoritative FUSE admission has been enabled.


## D25 lane-local authority checkpoint — Oct 6 2026

The user accepted lane-local authority before D25 activation. FileID/lane records
now persist geometry, epoch and history completeness. Cold bootstrap atomically
prepares inode/bitmap/history guards and the lane stamp/history/authority triple,
then uses durable COMMIT and normal transaction resolution. Shrink updates
installed lane authority together with its fence. Missing authority cannot grant
an epoch, and a retired history cannot keep a stale complete floor.

`LANE_WRITER_VIEW` (109/110) establishes one lane-group ReadIndex and reads no
inode or remote decision. Its client routes by the actual publication lane.
Tests exercise holes, FileID/geometry mismatch, partial resolution, aborted
fences, failed storage, bounded fence races and single-group host routing.

This stages the recommended foundation. FUSE admission and public logical
truncate remain disabled. Cold bootstrap RPC, missing-lane-only admission helper,
geometry-checked cache APIs and durable bootstrap crash recovery are now staged.
Next connect FUSE write callers and immutable epoch-owned flush plans, then
finish coherent retirement, live sweep and distribution acceptance gates.
[NUC ten-round checkpoint](../../results/measure/20261006-nuc-ten-rounds/SUMMARY.md). See the
[accepted route and remaining work](d25-admission-routing.md).

[Lane-authority verification evidence](../../results/measure/20261006-lane-authority/SUMMARY.md).

## Xorinox retained W36 artifact repaired — Oct 6 2026

All three servers were current at d0e8dce4 when du reported the retained
`/posix-2c/peer_rename_vs_unlink_src/b`. Its directory timestamp matched the old
00:41:38 UTC occurrence. Prevention alone does not repair persisted names.
b4a75492 adds single-shard guarded unlink cleanup; the name was removed through
Raft, server readdir is empty, and full-export du now completes without errors.
The current xorinox build passed the rename-vs-unlink gate 20/20 after repair.
Directory/foreign-shard orphan recovery remains outside this narrow fix.
[Evidence and scope](../../results/measure/20261006-xorinox-orphan-unlink/SUMMARY.md).

## D25 durable publication recovery checkpoint — Oct 6 2026

The accepted next step adds canonical per-publication identity, atomic durable
outcomes, lane-routed submission/status RPCs and staged cache result handling.
Retries recover the same intent after lost replies or superseding writes;
UNKNOWN retains ownership, exact rejection retains bytes but releases its token,
and exact success acknowledges only its snapshot. Legacy REPORT and active FUSE
flush paths are unchanged. [Validation evidence](../../results/measure/20261006-durable-publication/SUMMARY.md).
See [implementation and remaining activation gates](d25-admission-routing.md#durable-publication-results--implemented-staged).


## D25 receipt retirement checkpoint — Oct 6 2026

Explicit lane-routed retirement, exact digest verification, a 64-receipt
per-stream admission cap, and atomic durable replay floors are implemented and
staged. Retirement follows stored sequence order. Replays below the floor return
RETIRED, which never authorizes dropping or rebasing dirty ownership. Linux
build, metadata/session, publication transport, cache retention and durable
recovery regressions pass. Public FUSE flush integration remains disabled.
The per-stream bound is not a global bound: ordered client ACK retry ownership
and I23 session admission/fencing before abandoned-stream cleanup remain required
before activation. [Evidence and remaining gates](../../results/measure/20261006-publication-retirement/SUMMARY.md).
