# ORCD investigation: findings and repair hypotheses — W109/W110

**Assigned cluster:** `orcd-test`. **State:** investigation open; no repair or
new live correctness gate performed. October 11, 2026 UTC; runtime `52bc8edb`.
The inspected production sources match devel at `5be706e1`. Investigation used
saved profiles/counters/traces and read-only historical logs; live workload,
installed binaries, configuration and mounts were preserved.

## Priority and confidence

| Priority | Finding / hypothesis | Confidence | Next action |
| --- | --- | --- | --- |
| 1 — W109 / W92 | A 1,024-change incremental limit forces repeated full query refolds under sustained creation. | Strong code/evidence match; exact fallback reason not recorded. | Reproduce the threshold, then design bounded processing of larger deltas. |
| 2 — W109 / W23 / D30 | Query work and compaction compete with foreground metadata progress. | Correlation; responsible queue/lock/storage phase unproved. | Attribute request delay before changing scheduling or lock scope. |
| 3 — W110 | The two TCP retransmissions may be duplicate tail probes following delayed/reordered ACKs. | Consistent counters; per-flow causality unproved. | Correlate sequence/ACK evidence with EFS operation outcomes. |
| 4 — W109, GC follow-up | Repeated lane-sweep BUSY logs can represent bounded delete progress rather than a stuck apply entry. | Code confirms semantics; physical progress unmeasured here. | Check remaining chunk/object counts and reaper fairness. |
| 5 — W110 | A stuck RDMA send may consume more of a request deadline than expected. | Code-audit hypothesis only; no observed stuck completion. | Audit deadline propagation and test a private stalled-QP fixture. |

## 1. Repeated full query refolds: the strongest repair lead

Each host published 14 query generations in the saved log window:

| Host | Generations | Raw partitions rebuilt per publication | Inode rows refolded per publication | New files between publications | Snapshot pin time |
| --- | --- | --- | --- | --- | --- |
| fcstor004 | 302–315 | 18–40 | 1,040,812–1,075,905 | 1,334–3,013 | 0–15 ms |
| fcstor005 | 297–310 | 16–36 | 1,043,009–1,076,843 | 1,538–3,024 | 0–7 ms |

Raw partition reuse therefore does **not** establish incremental derived-view
reuse. Ordinary pre-strace builds take about 7.1–7.5 seconds, during which about
2,800–3,000 new files accumulate. CPU profiles independently place substantial
work under `efs_query_disk_build` (41.12% inclusive on fcstor005).

[Incremental builder](../../src/meta/query_disk_incremental.h) defines
`CHANGE_MAX=1024` for both unique changed inode IDs and dirty directories.
`changed_add` or `dirty_add` returns `INCREMENTAL_FULL` when its bounded set is
full; missing directory coverage can also request that fallback. New inode rows
enter the changed-ID set. The [build dispatcher](../../src/meta/query_disk.c)
then discards candidate derived files and calls `nodes_build` for all shards,
followed by `folds_build`. This explains how a few changed raw partitions can
trigger a million-row refold. It is a resource/coverage fallback, not an I/O
error fallback; actual errors must remain errors.

**Hypothesis:** ordinary write volume repeatedly exceeds the incremental change
budget, creating a feedback loop: full build takes seconds, so the next delta
also exceeds the limit. The logs do not expose fallback reasons; missing
coverage or other conditions could explain some generations. This is not yet
proof that it caused earlier lookup EBUSY or query timeouts.

**Test before repair:** in a private direct-I/O/native-RDMA ORCD fixture, compare
1,023 / 1,024 / 1,025 unique inode changes from an already published view, then
sustained creation at the observed rate. Record fallback reason, changed/dirty
set cardinalities, per-phase time, candidate bytes and foreground request p95/
p99. Separately exercise dirty ancestor closure exceeding the directory budget.
The hypothesis weakens if large deltas stay incremental or coverage failures,
not budget exhaustion, dominate the reproduction.

**Repair direction:** bounded disk-backed changed-ID/ancestor work sets or
bounded passes over one immutable candidate could process routine large deltas
without refolding the entire namespace. Keep explicit memory/disk/work budgets,
cancellation, generation consistency and atomic publication. A larger constant
alone moves the cliff and increases memory without resolving its scaling.
Existing two-second refresh coalescing already exists; adding that mechanism
again would not address this evidence.

**Acceptance:** complete result equality against full rebuild for creation,
delete, rename, hard-link aliases, ancestor aggregates, timestamp extrema and
permission filtering; boundary/resource/cancellation tests; lower sustained
refold work and foreground latency; unchanged full native/peer gate oracles.

## 2. Metadata waits: locate the contention rather than assume a lock

fcstor004 has 15 apply waits of 21.668–50.117 ms, median 23.895 ms.
Twelve wait intervals overlap a local compaction interval. Compactions are
frequent, so overlap alone is weak causal evidence. Snapshot pinning is 0–15 ms;
full query building runs outside the host mutex, and background compaction calls
`kv_compact_locked(l,1)`, dropping the KV mutex during file merge. Point lookup
also pins segments and drops the mutex for reads. Claiming that the entire
seven-second query or compaction holds the metadata lock would be incorrect.
Foreground flush/install, snapshot copying, scheduling, durability and admission
remain plausible contributors.

**Next measurement:** use existing `EFS_RAFT_OBS=1` and `EFS_RPC_PROF` diagnostics
in a private fixture or a separately coordinated deployment. They are cached
at startup; no live restart was performed here. Correlate checkout/send/receive/
BUSY backoff, sender failures, pump lock wait/hold, apply/persist phase maxima,
L0/flush pressure and storage sync timing with request identities. RPC receive
wall time includes server processing; it is not pure wire RTT. Compare otherwise
identical load with query refresh enabled/disabled in private fixtures, keeping
namespace correctness checks and deadlines unchanged.

**Conditional repair:** if query work dominates, budget refresh I/O/CPU and fix
item 1; if mutex hold or foreground flush dominates, shorten the demonstrated
critical section or improve admission while retaining durability ordering.
Do not increase RPC timeouts or bypass authoritative reads to make gates pass.
Keep W23/D30 broader storage-pressure obligations separate.

## 3. TCP retransmissions are not demonstrated permanent message loss

The first fcstor004 counter interval adds one `RetransSegs`, one
`TCPLossProbes`, one `TCPDSACKRecv` and one `TCPDSACKRecvSegs`. fcstor006 adds
one of each over the longer interval, plus two `TCPSACKReorder` events.
fcstor007 adds one `TCPDSACKOldSent` initially and two cumulatively. EFS socket
snapshots identify one server→client retransmission on each server. Extended
counters are host-wide: they cannot prove that every probe/DSACK belongs to
those sockets. No TCP timeout, reset or selected native-IB error counter growth
was observed.

D-SACK reports duplicate received data; it can support an inference of an
unnecessary retransmission, but delayed/lost ACKs and packet reordering remain
possible ([RFC 2883](https://www.rfc-editor.org/rfc/rfc2883.html)). Tail loss
probing is a recovery mechanism, not proof of permanent application loss
([RFC 8985](https://www.rfc-editor.org/rfc/rfc8985.html)). These observations
are consistent with duplicate probes; they do not establish their cause.
The fcstor004 retransmission predates strace; the later fcstor006 event falls
in a window including client tracing, so observer effects cannot be excluded.

**Next test:** during a reproduced stall, correlate EFS operation identity,
attempt, deadline and outcome with per-flow sequence/ACK/DSACK timing and QP/CQ
state. Use bounded header-only capture or suitable tracepoints if access permits;
avoid payload capture. Retry/recovery must preserve acknowledged cold bytes and
ambiguous-operation semantics. Selected IB counters remaining flat does not
exclude microbursts, switch congestion or short receive starvation. There is no
basis here for changing TCP retry/congestion settings or declaring IB loss.

## 4. GC BUSY: distinguish partial progress from an error storm

Historical logs from **04:33:30–04:34:30 UTC** show lane-sweep `rc=-13` for
inodes 30447, 39615, 43395 and 840179. Each inode has 128 distinct advancing
Raft indices per hosted group; dual-group hosts fcstor004/005 show 256.
Replica log rows must not be summed as unique cluster proposals. These are not
repeated observations of one fixed apply index.

[Batch application](../../src/meta/meta_apply.c) deletes the selected chunk rows
and writes their GC references, then returns `EFS_ERR_BUSY` if more chunks
remain. The [reaper](../../src/server/raft_host.c) treats BUSY as partial
progress, advances other lanes and retains the marker until every lane is done.
Thus the repeated message is compatible with bounded processing of large
previously deleted files. Advancing indices alone do not prove chunk-count or
physical-byte reduction; selection contents and reclamation outcomes were not
captured in this investigation.

**Next test / conditional repair:** record selected/remaining chunk counts,
queued GC references, physical object/byte reclamation and foreground wait
latency during a large delete and restart. If progress occurs, distinguish
nonterminal progress from real storage/authority failures in diagnostics. If
counts stall or reaper work starves foreground work, repair the demonstrated
selection/fairness issue. Never translate all BUSY replies to success or remove
a reap marker before lanes and unpublished-object cleanup are complete.

## 5. RDMA retry deadline audit: lower priority, not a diagnosed failure

Native transport defaults to unbounded hardware RNR retry, but
[RDMA implementation](../../src/common/rdma.c) has a five-second overdue-send
check (`EFS_RDMA_SEND_WAIT_US`), completion polling and bounded reply waits.
Therefore unbounded hardware retry does not imply an indefinitely blocked EFS
caller. Audit whether every send-pool/completion wait consumes the caller's
remaining deadline, especially the Raft sender's shorter budget. Only a private
stalled-QP/no-receive fixture can establish an actual deadline violation.
If one is reproduced, propagate remaining deadlines consistently, safely tear
down dead QPs and retain replay identity; do not blindly reduce hardware retries.

## Evidence and limits

Derived query/wait/TCP records (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-communication/hypothesis-evidence.json`)
and historical lane-sweep records (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-orcd-communication/historical-failure-window.json`)
supplement the [baseline capture](orcd-communication-20261011.md).
Raw profiles/traces remain in that capture's ORCD scratch directory.

The 20.503-second fcstor004 build and 12.799/12.462-second fcstor005 builds
overlap strace; do not use those outliers as unperturbed latency measurements.
Perf CPU percentages are sampled CPU attribution, not request latency, and
inclusive chains overlap. Earlier full-load result remains 1485 pass, 27 fail,
7 skips, 28 NOTRUN. No item or acceptance gate was closed by this investigation.
