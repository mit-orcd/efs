# Queue evidence review — Oct 7, round 2

[Current status](../status/README.md) · [Current handoff](../status/in-flight.md)

Review window: 14:42–14:57 UTC, with a 15-minute limit. Local HEAD was
`ddc677ae`; pre-existing working source changes were inspected but not modified.
This is source/retained-evidence reconciliation, not a fresh cluster test run.
Historical builds below do not identify current running processes.

## Accepted records moved out of the active queue

| Identity | Retained evidence | Acceptance scope |
| --- | --- | --- |
| W36 | [Final NUC race log](../../results/measure/20261007-roadmap-rounds/final/final-w36.log), [xorinox repair/acceptance](../../results/measure/20261006-xorinox-orphan-unlink/SUMMARY.md) | 20/20 named rename-versus-unlink repeats on NUC `128f6b7d` and xorinox `b4a75492`; repeat on changed rollout |
| Row 0m / associated W57 fix | [Full single-client TSV](../../results/measure/20261007-roadmap-rounds/final/tsv/posix-20261007-050224.tsv), [full peer TSV](../../results/measure/20261007-roadmap-rounds/final/tsv/posix2c-20261007-050327.tsv) | seven `dir_times_*` tests PASS, including rename and child mutation; peer directory-mtime visibility PASS; supersedes 6/7 and peer-pending state |

W56's four root-rename tests also pass in the single-client TSV. Its separate
xorinox rollout obligation is retained; these NUC results do not close it.

## Source and evidence reconciliation

| Item | What the review establishes | What remains |
| --- | --- | --- |
| W42 | Both FUSE statfs and management status call `efs_capacity_logical`; [four-node status](../../results/measure/20261007-roadmap-rounds/final/final-status.log) reports 500 GiB from 4 × 187.5 GiB quotas and matching FUSE total | Named fcstor gate not established here; one quota-rejected stripe member can leave protection debt, requiring fault/repair evidence |
| W27 | Both report paths distinguish ownership-free phantom marks from accepted local work; `febc55e5` is recorded in the item | Nonzero-node staging identity fallback and current traced putid-miss gate remain |
| W52 | `host_resolve_caught_up` still loops over OPEN reservations and calls `host_propose_wait` individually | Approved shape A batching is not implemented; ordinary append passes are not its latency/proposal gate |
| W43 / D25 | Public `efs_client_truncate` still flushes/reports and calls SETATTR; it does not invoke staged logical resize | Epoch-owned FUSE admission/publication, live-file sweep/history retirement and activation gates |
| W61/W62/W63 | Exact cleanup apply verdicts, whole-record delta capacity, diagnostics and restart export changes are visible in the working tree; tests now include verdict/storage/live reclamation fixtures | Test-file existence is not test execution; rollout, original incident cause and lost-reference reconciliation remain unverified in this round |
| W64 | Recorded `ec1500ec` fixes host-local replay races; round-20 loss trace remains retained | Durable replay across leader changes remains unresolved |
| W66 | `g_group_leader[2]` and `HOST_NGROUPS=2` still implement two-group topology | Distributed leadership/routing and measured capacity remain enhancement work |
| W67/W68 | Body defaults and worker-owned reply destructors agree with current documentation | Targeted pressure/failure/RSS and real worker-retirement acceptance remain |
| W38/W54/W55/W58/W59/W60 | Existing recorded fixes/investigations and limits were cross-checked against the queue; no new named acceptance evidence establishes closure | Keep the traced cold-data, publication-cause, sustained-write or xorinox gates as the item states |
| W48/W53 | Evidence requests concern original harness exits or matched storm latency | New general throughput/clean POSIX results do not answer those investigations |

## Operator-document corrections

- The soft `EFS_DCACHE_BYTES` target is 128 MiB, not 2 GiB. Separate hard
  admission and drain defaults are 256 MiB and 64 MiB; they are not RSS limits.
- `EFS_FAULT_WITHHOLD` exists in fault-enabled clients, before REPORT
  serialization. `EFS_FAULT_REJECT_PUBLISH` remains a proposed server hook;
  no implementation was found in the reviewed source.
- `--quota` is node-wide across its roots, not an independent per-root limit.
- EFS payload usage and physical filesystem blocks differ; unlink GC is
  asynchronous. Quiet GC logs do not prove a drained queue.
- SHRINK_QUOTA reduces the running quota and refuses below usage. No migration
  worker is started, although `cmd_shrink_quota` prints that migration started.
  This operator-facing defect is **W70**, indexed with its change/gate; source
  was left unchanged. Existing capability documentation already lists
  evacuation/rebalance as absent.
- The read-verification variable no longer promises a background healing
  scrubber: client re-hashing is opt-in, server GET checks stored digests, and
  automatic repair remains a capability gap.
- The fcstor topology/commands are identified as a recorded deployment rather
  than the current universal controller for NUC or xorinox.

- Capability inventory corrected: fault simulator/integration, WITHHOLD unit
  coverage and a live GC fixture exist. Their existence does not establish
  current execution or complete fault acceptance; the rest of the older
  capability inventory is explicitly marked for re-audit.
- Documentation-index directory links now name concrete files; the archive
  index defines closure by its recorded scope rather than an unconditional
  prohibition on revisiting a finding.

## Review still remaining

Approximately **50% of the documentation review remains**, with uncertainty
of roughly ten percentage points. This estimates review effort, not open bugs,
implementation completeness, or percentage of passing tests.

Covered: active status organization, index/identity consistency, key queue
source checks and retained correctness acceptance, plus several concrete
operator corrections. Remaining: full decision/spec/implementation consistency,
complete operating/quickstart command verification, capability inventory and
performance evidence consistency, then final cross-page closure/numbering check.
Archive history is outside the primary review scope.
