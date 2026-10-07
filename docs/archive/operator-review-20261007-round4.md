# Operator/documentation review — Oct 7, round 4

[Queue](../status/README.md) · [Operations](../operations/operations.md)

Review started 15:16:11 UTC with a 15-minute limit, at HEAD `ddc677ae` plus
existing working changes. Documentation/source/evidence review only: no
production source edits, process signaling, installations, cluster mutations
or runtime test execution. Preexisting/concurrent source changes were left
alone. The root README was also corrected after obtaining filesystem access.

## Indexed source findings

| ID | Inspected path | Finding and review limit |
| --- | --- | --- |
| [W78](../backlog/work-items.md#w78) | `scripts/server.sh:kill_from_pidfile`, `kill_pid_graceful`, start/path-stop | PID-file signaling lacks process identity validation. A stale/reused PID can target an unrelated process; no live reproduction was performed. |
| [W79](../backlog/work-items.md#w79) | mgmt help/aliases/`cmd_raft_mkfs`, metadata mkfs apply, host salt readback, FUSE bootstrap | Help advertises a name that the single-export handler ignores. Existing-root initialization is idempotent and uses the committed salt; no initialization or uncertain-reply test was run. |
| [W80](../backlog/work-items.md#w80) | handler QUERY_STATS and `src/query/efs_query.c` | A zero-filled retired query is printed as successful totals. Zero does not establish an empty cluster or completed reclamation; no live query was run. |

A final directory-path check also indexed [W81](../backlog/work-items.md#w81):
size-triggered spread and its migrator exist, but the accepted automatic
pressure trigger is unwired and its policy bound remains unspecified. Ancestry
guards exist in the rename host; their presence is not blanket depth/concurrency
acceptance. Future local-engine benchmark gate commands now use `efs-bench`;
historical `efsd --bench` measurements retain their original labels.

Each has one canonical queue row and detailed next action/acceptance gate.
They do not establish a new execution priority or diagnose the GC incident.

## Guidance corrections and preserved records

- Quickstart/scripts: optional RDMA dependencies, three distinct roots,
  single-export initialization/retry meaning, all three stop commands,
  successful client drain before server shutdown, parser-specific recorder
  option positions. Server cleanup escalates after roughly three seconds;
  it is not a guaranteed graceful shutdown.
- Storage: roots have different data/metadata/membership/log roles; direct
  writes have a 4 KiB digest tail while buffered writes append 32 bytes.
  O_DIRECT is not a persistence guarantee. The former wipe-as-upgrade advice
  lacked a verified migration gate and is withdrawn; existing stores must
  be preserved until compatibility and data migration are established.
- Mount knobs: splice is off by default, so the old mandatory 8 MiB pipe
  sysctl advice was removed. No copy-saving benefit is accepted from the
  recorded profile. Timestamp default/disable and wrapper versus binary
  recorder output variables are now distinguished.
- Capability inventory: external NFS re-export and userspace benchmark/mgmt
  paths exist; a packaged general application client is not established.
  Placement salt is not authenticated cluster identity. Targeted probes are
  not a full fsck. Typed counters are distinct from a time-series service.
- NFS: the Oct 5 EIO/re-resolution symptom is preserved, while the suggested
  libfuse-table explanation is not treated as proven. Low-level entries use
  EFS inode IDs and generation 1; trace kernel export/decode and gateway
  lifetime before selecting a fix. No current restart acceptance is claimed.
- Counters: p50 covers the last 64 samples, uptime starts at first measured
  operation, and relaxed snapshots are diagnostic. Portal negative-delta
  detection is not a unique process identity. No universal <0.1% overhead
  conclusion follows from the noisy VM repetitions.
- Performance: historical flush-in-clock results are not physical power-loss
  acceptance. The corrected latency study separates population from timing;
  per-run p99 averages are not pooled percentiles. Engine/cluster results
  remain separate, with original command labels retained.
- Design rationale no longer calls the production table wholly in-memory;
  quorum reasoning is conditional on durable storage assumptions and does
  not close W71 or the EFS composition gates.

Full superseded records remain linked:
[product surface](product-surface-before-round4-20261007.md),
[measurement runbooks](measurement-runbooks-20261007.md), and
[Oct 4 io-stats observations](io-stats-acceptance-20261004.md).
No accepted decision was revoked or cluster data touched. The active runbook
is a compact entry point; its former dated tool/approval commands are history.

The final parked-test reconciliation also removed the obsolete cross-group
directory-utimens EINVAL diagnosis and fixed test counts from the active ideas
page. Current source has cross-group lane fencing; exact explicit-utimens
acceptance remains distinct from closed parent-directory timestamp gates. The
[old checklist](parked-testing-checkpoint-20261007.md) is preserved intact.

## Remaining review estimate

Approximately **15% of documentation review effort remains (±10 percentage
points)**. Main remaining work: deeper directory/transaction and earlier
accepted-decision cross-checks, external-system comparison/naming claims that
need current primary-source research, and a final evidence/closure reconciliation
across older work-item details. Link validity does not prove every historical
measurement or architecture assertion. Implementing the open findings and
running live restart/corruption/power-loss gates are separate, larger tasks.


## Validation

All five architecture documentation gates passed. Expanded local links/anchors
passed across 38 documents (active pages, root README and five new archive
records); canonical W61–W81 index/detail uniqueness and diff whitespace passed.
Generated architecture artifacts are current. No runtime tests were executed.
