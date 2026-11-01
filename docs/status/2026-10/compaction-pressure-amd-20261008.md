# D30/W23/W89 — AMD admission and follower pressure

Branch `devel-compaction-pressure`, merged baseline `8086c2e9`, implementation
`f6d48828`. The user authorized continuing from the merged W66/pressure baseline
on `amd_efs`. No shared checkout or other package's files were changed.

The integrated implementation and finite AMD acceptance gates pass. **Production
activation remains open:** `EFS_ADMISSION_PRESSURE=1` enables admission and
leadership eligibility; its default is off pending the accepted D29/W88 ordering
and review of expansion charges across metadata operation families. This report
does not close D30 as a universal process-memory or permanent-stall throughput
proof. W23's real-cap/follower/recovery experiment and W89's schema validation
now have valid evidence for this implementation, replacing no historical logs.

## Implemented behavior

Admission checks run before local proposal persistence and before a follower
accepts a new AppendEntries suffix. Exhaustion returns BUSY without accepting
that suffix; poisoned KV persistence returns IO. Duplicate log entries and
empty probes retain log matching. Committed apply is never rejected for L0
pressure. Existing durable ACK ordering, quorum rules, member fencing and
all-member GC acknowledgements remain intact.

Accepted operation ownership is the durable log's `(group,index,term)`, not an
RPC ticket. Exact unapplied count/command bytes remain charged until actual apply,
definite suffix truncation or installed snapshot. RPC timeout/cancellation,
forwarding failure and leader changes cannot free those bytes. Disk prefix
accounting supports constant-time byte queries; replay rebuilds it without a
format change. Execution reservations separately bound synchronous proposal and
verdict waits, forwarding and fsync admission. They release when execution exits,
without releasing possibly committed log ownership.

Pressured followers reply with a failure marker, receive data retries after two
heartbeat intervals, and continue receiving empty election/read probes. A pressure
failure grants neither replication progress nor a ReadIndex acknowledgement.
Low-capacity replicas cannot campaign; a pressured leader relinquishes its role.
Existing log-window and streamed InstallSnapshot recovery remain available.
No compaction or pump scheduling redesign was introduced.

| Resource/policy | Explicit limit |
| --- | --- |
| L0 local proposal/leadership soft watermark | 768 MiB |
| L0 follower receive hard watermark | unchanged 1 GiB |
| Memtable ordinary stop / all new admission stop | 32 MiB / 64 MiB |
| Global unapplied charge | 256 MiB, including 32 MiB recovery reserve |
| Charge formula | 16 × command bytes + 4 KiB per entry |
| Global unapplied entries | 2,048, including 256 recovery slots |
| New log command bytes across hosted groups | 1 GiB, including 256 MiB recovery reserve |
| Concurrent proposal execution | 64 / 8 MiB; 8 slots / 1 MiB reserved |
| Inbox queue | 256 frames / 32 MiB, checked before allocation |
| Outbox queue per peer | 2,048 frames / 16 MiB; 64 KiB kept for control traffic |

Only bounded internal DECIDE, RESOLVE, DROP, LANE_SWEEP, REAP_DONE, GC_ACK and
PUBLICATION_RETIRE commands can use recovery reserves; recovery commands must be
at most 256 KiB. They still obey total count/byte/log and hard memtable limits.
These are admission accounting limits, not an RSS identity. Dequeued inbox
cohorts, sender batches, decode buffers, retained applied log windows, KV views
and allocator retention add separately bounded or existing storage to queues.
Physical log bytes are measured separately from the new-command-byte counter.
The 16×/4 KiB charge is conservative for the exercised workload; a complete
fan-out/retained-state audit remains required before making it a general
metadata-memory guarantee.

## AMD evidence

Raw evidence and measured summary (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-amd-compaction-pressure/summary.json`)
come from `/data1/efs/evidence/compaction-pressure-20261008/cap-05`, three private daemons,
direct I/O/TCP, synchronous production KV/log persistence, Linux user `efs`.
All `.efs` commands were disabled. A compile-only hook inserts 1 MiB test values
through real Raft into a private KV namespace. Sixteen repeatedly updated keys
keep healthy live state small while the third replica's real compactor is parked.
Neither the L0 cap nor production watermarks were reduced.

- 2,240 acknowledged 1 MiB pressure writes; 1,024 continued during the held-cap
  phase. The sampled phase lasted 35.17 seconds.
- Stalled follower L0 reached 1,075,860,730 bytes / 259 segments. Its received
  suffix rejection count reached 1,002; sampled replication lag reached 1,431
  entries. Local commit-minus-apply lag remained zero: these are different metrics.
- Follower sampled peak RSS was 1,599,424 KiB; maximum across replicas was
  1,674,504 KiB. The fixture's explicit per-replica RSS gate is 2 GiB.
- Largest sampled physical log was 2,425,356,288 bytes, below the explicit
  4 GiB per-replica fixture gate. Follower physical KV peaked at 1,092,677,273
  bytes. Physical files are not the new-log admission counter.
- Inbox high-water was at most 4,194,708 bytes; summed per-peer outbox
  high-water at most 7,340,427 bytes. Sampled pending work peaked at one
  1,048,585-byte command; saturated policy boundaries are exercised separately.
- Pressure write ACK latency: p50 10.679 ms, p99 21.122 ms, maximum 1.568408 s;
  no client BUSY retries in this sequential producer gate. The fixture separately
  requires every ACK latency below 15 seconds.
- During the real cap stall, 32 files of 256 KiB were written and fsynced through
  FUSE. Maximum recorded fsync duration was 10.648 ms. Eight temporary files
  were created, fsynced and unlinked during the same stall.
- The stalled replica performed successful InstallSnapshot import. After fault
  release all three replicas reached equal commit/applied positions; the sampled
  recovery interval was 4.23 seconds, within the 180-second fixture deadline.
- Exact file bytes were read through the recovered peer, then through another
  peer after deliberate SIGKILL of **only the private fixture daemons** and cold
  restart. All sixteen final pressure values also passed exact cold KV reopen
  on every replica. No installed daemon was crashed.

The strict TSV (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-amd-compaction-pressure/cap-05/samples.tsv`)
passes `w23_samples.py cap-proof`: simultaneous complete three-replica ticks,
known roles/RSS/L0/lag and a real follower at the unchanged cap. Raw JSON includes
queues, physical files and group positions. Unobserved apply-sleep counts remain
`NA`; they are not zero. The summary explicitly describes finite sampled peaks.
Source and binary identities are retained in
source checksums (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-amd-compaction-pressure/source-sha256.txt`).
The private rsynced binary's `unknown` version is not used as source identity.

## Validation and restoration

The full AMD test suite (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-amd-compaction-pressure/full-tests-final.log`)
passes with pressure enabled and faults compiled. Raft tests cover admission
before persistence, nonacceptance under BUSY, healthy quorum progress, paced
retries, follower convergence, accepted-log ownership with both peers absent,
release only after apply, and leader replacement. Existing persistence-failure,
ReadIndex, storage, snapshot and replay tests remain passing. Raft ASan/UBSan
also passes locally. The compiled host policy test covers soft/hard receive
boundaries, recovery count/log/byte reserves, persistence poison and leadership
eligibility; it passes on Mac and AMD and is part of `make test`.

The publication pending regression (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-amd-compaction-pressure/publication-pending.log`)
passes lost admission ACK, four-private-daemon crash, cold pending recovery,
altered payload refusal, final rejection and durable retirement floor. This
validates coexistence with staged D29 machinery; it does not establish complete
active production REPORT receipt coverage.

Installed `/data1/efs/n1..n4` stores and daemons were untouched. The second
approved mount was borrowed only after clean drain and restored with the exact
installed binary, TCP/direct settings and disabled `.efs` commands. Both installed
mounts serve again; all installed daemons and clients run as `efs`. Restoration
and final health evidence are retained. The AMD advisory reservation was released
only after restoration and evidence capture.

## Remaining activation obligations

1. Complete D29/W88 active publication receipt acceptance before enabling the
   D30 policy by default, as required by the accepted decision order.
2. Audit metadata expansion and recovery-command fan-out against the explicit
   charge; validate sustained concurrent saturation beyond the sequential
   production-cap producer. This gate proves actual bounds for its workload,
   not a universal process-memory bound.
3. Review deployment limits against the target cluster's memory/storage budget.
   No indefinite write-throughput claim is made while a replica remains stalled.

## Installed AMD deployment after host return

At the user's explicit request, deployed commit `10cbfaf8da91` from this worktree
using `~/git/devops/nuc-efs/deploy.sh` with `EFS_CLUSTER=amd`. AMD was already
stopped and unmounted after returning; no forced stop or discard was used.
Existing stores and format marker were preserved. Previous binaries, client
scripts, devops scripts and a bundle of the previous installed source HEAD were
saved under `/data1/efs/evidence/deploy-backups/pressure-10cbfaf8`.

The installed build identifies branch `devel-compaction-pressure`, commit
`10cbfaf8`, build time `2026-10-09T01:15:32Z`. Four daemons and two clients run
as Linux `efs` (UID 1001), direct I/O/TCP, with explicit `EFS_COMMANDS=0`,
`EFS_COMMANDS_SERVER=0`, `EFS_ADMISSION_PRESSURE=0`. The opt-in pressure gate is
unchanged; installing this build does not resolve the activation obligations above.

Installed acceptance passed:

- Single-client POSIX: 213 passed, zero failed, four skipped (unsupported mmap
  plus three intentionally disabled `.efs` command tests).
- Peer-client POSIX: 64/64 passed.
- Persistence: 26/26 prepare and 26/26 exact verify after clean client remount.
- Final health: both mounts serving, all six processes owned by `efs`, matching
  installed branch identity, both groups converged at commit=applied on all
  three hosted replicas.

Deployment logs, TSVs and final health (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-amd-compaction-deploy/health.json`)
are retained alongside `results.json`, build/start logs and per-suite output.
The advisory reservation was released after health verification. The deployed
cluster remains running; this documentation-only follow-up commit does not
require rebuilding its binaries.

**Raw evidence note (2026-10-23):** the bulk data in `compaction-pressure-20261008`, `deploy-backups` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
