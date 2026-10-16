# ORCD findings suitable for smaller-cluster development — October 13, 2026

Current human scope: correctness and blocked acceptance only. The active
[sort-cleanup/temporary-peak fix](query-sort-peak-correctness-20261013.md)
retains16/32GiB limits. Earlier format-compaction suggestions below are
historical; no further compression or performance tuning is in this work.


Several outstanding fixes or integration checks can be developed without a
10-million-file namespace. Small regressions do not replace ORCD acceptance.
This note follows [the ordinary-file `.efs` scope](devel-efs-20261012.md) and
[the detailed ORCD evidence](orcd-efs-400g-20261012.md); it does not create
copies of existing W items or close their broader gates.

## Current execution and limits

Exact tested and deployed source: `419aa90f367ace783bce790f4a2f4d580320077f`
on `devel-efs`, based on `release-v0.3.2-pre-alpha`. ORCD uses four servers
(fcstor003–006), seven clients (fcstor007–013), native hardware RDMA through
mlx5_2, direct-I/O files storage, `.efs` enabled.
Processes run as the site's nonroot erbmi1 account. Budgets and deadlines are
unchanged. Expiry and unplanned-event repair/recovery remain deferred.

Complete normal native, fault-enabled native, eleven ASan/UBSan host harnesses
and sanitized Raft normal/early-ACK prerequisites PASS. Deployment/source/profile
checks and exact acknowledged small/large markers on all seven remounted clients
PASS. All eleven hosts match 791 code/test inputs. Clients drained normally,
without force. Server readiness took 284.704 seconds within the 300-second gate.

**Mounted command acceptance FAIL:** all six focused suites (`efs_commands`,
`efs_command_options`, `efs_command_pages`, `virtual_interfaces`,
`efs_commands_load`, `query_large_delta`) encounter ENOSPC reading `.efs` query
files. The peer unmounted normally. Subsequent concurrent, same-source cold
restart, full 400 GiB and loaded-latency acceptance are NOTRUN for this source.
Do not transfer the earlier full 400 GiB PASS at `62770d5c3bfd` to this build.
The subsequent inventory isolates a generation-size ceiling: the old leaked view is gone, the checkpoint occupies 13,018,089,332 logical bytes, and refresh is rejected during raw/index construction near 11,085,042 accumulated inode rows. The unchanged 16 GiB generation limit is tighter than the remaining aggregate headroom. A scaled native test reproduces the same EFS_ERR_QUOTA refusal with 3,000 files; it is not evidence of a universal 10-million-file threshold.

The user additionally ran seven 100 GiB scratch-to-EFS copies and one 100 GiB
EFS-to-EFS copy. All eight destinations reached the expected logical size;
these size checks are not complete payload verification. Several tiny-file
benchmarks failed or could not resume on the preceding source. Their screen
errors and retained files remain evidence, not successful runs.

## Development candidates

| Finding / existing scope | Why a smaller target helps | Small reproduction or next experiment | Outstanding work |
| --- | --- | --- | --- |
| Retained CLOSE admission starvation — W97 / D30 | One unresolved CLOSE and a newer unallocated CLOSE suffice; total file count is irrelevant. | `python3 tests/test_fuse_lease_retention.py`: actual bookkeeping/retry helpers, allocated identity priority, adoption ordering and a stale-key negative control. Then a private four-daemon/one-client mounted retry/drain test. | Source correction `3f41a4c4` is included in the qualified deployment. Loaded end-to-end retry and orderly-drain qualification remains open; do not claim W97 closed. |
| 512-byte EOF inflated to 128 KiB — ordinary-file publication / W97 | A reclaim PUT becoming reportable before the writer installs EOF is a scheduling race, rather than a file-count threshold. | `python3 tests/test_write_acceptance_gate.py`: production acceptance helpers, controlled early reclaim, shared writers, partial multi-chunk failure, deadline/full-slot checks and an early-REPORT negative control. Follow with concurrent 512-byte create/write/fsync/read/unlink on one mount under cache pressure. | Source correction `419aa90f` passes native prerequisites and is deployed. Exact-byte/EOF checks under loaded mounted I/O and after orderly restart remain open. Two failed files on the preceding source contain the correct 512-byte prefix followed by zeros out to 131,072 bytes; they are retained. |
| Startup catalogue generation retirement — W92 | Only a checkpoint, a replacement generation and an independently retained old reader are needed to test lifecycle ordering. | `tests/test_query_disk.c` through `make tests/test_query_disk && ./tests/test_query_disk`; exercise abandoned and successful catch-up, old-reader survival and retirement after the final handle closes. | Correction `46c05c05` passes native/negative-control evidence and is included in the deployed source. It does not close the latest mounted ENOSPC failure. |
| Latest query ENOSPC / publication integration — W92 / W109 | A 3,000-file native fixture already reproduces generation-quota refusal with a private 4 MiB limit. Cache record layout and accounting can be changed and compared without a large deployed namespace. | Run the scaled quota command below; qualify checked sort cleanup and actual simultaneous temporary charges, then repeat initial build, restore/catch-up, old-reader retention, fresh-subtree mutation, path/permission and rollup oracles. Measure actual and charged peak bytes, including sort scratch. | Correct temporary-peak accounting within the unchanged ORCD 16 GiB generation / 32 GiB aggregate limits. No further format compression or tuning. Budget rejection remains required when a candidate truly exceeds its limit; preserve temporary peak accounting, query semantics and deadlines. |
| Fork during ordinary direct-mode reads — W106 | The diagnostic fails after seven verified files, so failure does not require reading 10 million files. Its existing fixture is still large; a genuinely small-fixture reproduction is not yet proved. | Generalize `tests/live/efs_direct_read_fork.c` to a fresh small deterministic fixture. Compare eight read threads with and without the ninth thread that forks every 5 ms; preserve exact mismatch bytes. Use a Linux FUSE client with the same read mode. | Application fork/read safety remains OPEN. Removing forks from the capacity oracle fixes its methodology, not application compatibility. Other Linux clusters may help debug; final hardware-RDMA acceptance still belongs on ORCD. |
| Snapshot/log-rotation foreground stalls — W97 / D30, with W109 contention | Log rewrite cost depends on retained command bytes, not namespace file count. Ordinary xattr churn can reach the normal snapshot trigger with very few files. | `tests/live/efs_raft_rotation_latency.c` builds a fresh isolated retained-log fixture and verifies every record after replay. `tests/live/efs_snapshot_pressure.py` drives normal 512 MiB command thresholds without changing them; pair it with `efs_foreground_latency.py`. | Buffered/cached-CRC rotation improves isolated timings but does not close the 5.569/22.044-second original tails. Live rotation still reached roughly 3.6 seconds for a 1.52 GB retained image, blocking snapshot advancement. Investigate lock ownership, storage barriers and export/GC interference before changing scheduling. |
| Long orderly startup — W92 / W97 scale investigation | A size ladder and phase profiling can separate metadata replay/index work from physical fragment enumeration. A faster/smaller machine could otherwise hide the cost. | Profile normal startup with increasing independent inode, chunk-record and fragment-file populations. Capture phase durations, CPU, faults, I/O and resource peaks; keep acknowledged-data checks and the 300-second gate. | The observed 284.704-second readiness is PASS with little margin. Cause and a small reproduction are unproved; it is a performance investigation, not a new correctness failure. Full-size orderly restart remains an ORCD gate. |

## Measured small-fixture quota result

On fcstor006 at exact source `419aa90f367a`, the following standalone native
command PASSes its expected quota-refusal oracle. The environment applies only
to this synthetic test process; deployed daemon budgets remain unchanged.

```sh
cd /tmp/efs
EFS_QUERY_DISK_MAX_MIB=4 EFS_QUERY_DISK_TOTAL_MIB=8 \
EFS_QUERY_TEST_FILES=3000 EFS_QUERY_TEST_QUOTA=1 ./tests/test_query_disk
```

The builder returns `EFS_ERR_QUOTA (-11)` at raw shard 3950, with 3,004 inode
rows accumulated, before stats or persistence. This reproduces bounded
capacity refusal on a tiny namespace; it does not mean the quota check is
incorrect or that mounted acceptance passes. The cache inventory on the live
ORCD publishers contains one current 13.018 GB checkpoint and one temporary
build, rather than the earlier two completed generations.

Read-only debugger measurements on the deployed Linux binary give
`sizeof(disk_node)=584`, `sizeof(efs_name_entry)=288` and
`sizeof(disk_alias)=24`. The full 288-byte directory link is embedded in every
derived inode row, but regular-file rows do not use it. Replacing it with a
16-byte validated directory reference would save 272 bytes per inode in this
record alone (about 2.79 GiB at 11 million rows), before other index costs. This
is a proposed cache-format change: directory rename, hashed/local alias
selection, retained snapshots, restore/version transition and authority
validation need small native regressions before any rollout. No authoritative
metadata or ordinary-file payload format change is proposed here.

## Required outstanding `.efs` work and cluster assignment

The [three-package feature matrix](three-work-packages-20261013.md) now assigns
this enabled `.efs` development to AMD and final scale acceptance to ORCD.
Intel handles ordinary-file work with `.efs` client/server disabled. Expiry is disabled in all three packages.

All rows below remain required and OPEN. This is a development/acceptance
assignment, not evidence that AMD or Intel has run or passed these gates.
The latest mounted verdict remains FAIL at `419aa90f367a`; follow-up gates are
NOTRUN on that source. Earlier source-specific passes do not close these rows.

| Required work | AMD development/testing | ORCD requirement |
| --- | --- | --- |
| W92: checked cleanup and temporary-peak accounting for query ENOSPC within unchanged budgets | AMD Linux: small quota fixture, checked sort cleanup, initial build, accounting of temporary peak space, cache-format transition, restore/catch-up, directory rename/layout, permissions, rollups and retained-reader regressions. The existing 3,000-file test reproduces quota refusal; it does not yet prove the fix. | Final full-namespace rebuild/refresh under the existing 16 GiB generation and 32 GiB aggregate caps. All six focused mounted command suites must pass. |
| W109 / W92: concurrent-query correctness, freshness and timeout qualification | AMD Linux: concurrent ordinary-file mutations and `.efs` queries; reproduce contention and fix failures with existing deadlines and bounded resources. | Final seven-client concurrent queries against the full catalogue while ordinary-file I/O is active, using native RDMA/direct I/O. |
| W92 / W91: catalogue lifecycle and orderly restart | AMD Linux: retained readers, successful/abandoned refresh, checkpoint retirement, compatible restore or explicit derived-cache migration, clean unmount and restart, exact acknowledged payloads and post-restart queries. | Same-source full-size orderly restart and cold query/readiness checks within the unchanged 300-second gate, plus acknowledged-data verification on all seven clients. A cache-format migration does not substitute for a subsequent same-format cold restart. |
| W91 / W108: command behavior and integration regressions after these changes | AMD Linux: help/discovery, options/pages, `ls`/`find`, permissions, owner-name/numeric modes, default/human/JSON stats, feature transitions and truthful unsupported-expiry behavior. Existing owner-name passes remain historical evidence; rerun affected checks on the final source. | Final focused suites and default/human/JSON output checks on all seven clients at the real catalogue size. |
| W92 scale gate: full 400 GiB acceptance, latency and resource bounds | AMD Linux: smaller deterministic payload/path oracles, concurrent workloads and resource/latency instrumentation. Useful for development, but insufficient to close the specified scale gate. | Complete verification of all 1,638,400 payloads and exported paths (429,496,729,600 logical bytes), cold restart, loaded queries, latency and resource measurements on the final source/profile. No sampling or inherited PASS. |

AMD enabled-command development requires Linux FUSE and isolated stores, ports and
mounts; use the four logical server roles and at least one mounted client for
integration. Use RDMA/direct I/O where available. Explicitly labelled TCP logic
experiments may establish algorithmic correctness but do not qualify ORCD's
hardware transport. These assignments do not authorize changing another
cluster's active workload or imply that RDMA is currently available there.

Keep `.efs` enabled, ordinary files throughout
this package. Expiry and unplanned-event recovery remain deferred. The broader
CLOSE/EOF, fork/read and snapshot/log-rotation findings above can also be
developed on Intel with `.efs` disabled, but their loaded/platform acceptance remains
separate from completion of these `.efs` rows. Any broader platform or
billion-entry W91/W92 gates retain their own requirements; the 400 GiB run
does not close them automatically.

## Practical split

Use a private Linux development target with the current four logical server
roles and one FUSE client for mounted integration. Four daemons may share an
isolated machine for logic experiments, provided ports, stores and mounts are
separate from release services. TCP is acceptable for explicitly labelled logic
experiments where RDMA is unavailable; it does not qualify the requested
RDMA/direct-I/O profile. The extracted CLOSE and acceptance tests also run on
Mac without building the unsupported full Linux daemon there.

Run the deterministic lifecycle, retry and EOF tests first; next isolate the
latest query ENOSPC accounting, then develop a small fork/read fixture and
snapshot-overlap experiment. Keep full seven-client concurrency, real storage
latency, complete 1,638,400-file / 400 GiB payload and path verification, cold
restart, and resource bounds on ORCD. There is no measured universal
10-million-file failure threshold. Quota occupancy, retained log bytes,
concurrency and callback ordering are separate variables.

Raw prerequisites, workload inventories, screen/debugger evidence, query
failure logs and resource samples live outside Git under
`operations/devel-efs` and `/home/erbmi1/orcd/scratch/efs/orcd-test`.
