# ORCD findings suitable for smaller-cluster development — October 13, 2026

Several outstanding fixes or integration checks can be developed without a
10-million-file namespace. Small regressions do not replace ORCD acceptance.
This note follows [the ordinary-file `.efs` scope](devel-efs-20261012.md) and
[the detailed ORCD evidence](orcd-efs-400g-20261012.md); it does not create
copies of existing W items or close their broader gates.

## Current execution and limits

Exact tested and deployed source: `419aa90f367ace783bce790f4a2f4d580320077f`
on `devel-efs`, based on `release-v0.3.2-pre-alpha`. ORCD uses four servers
(fcstor003–006), seven clients (fcstor007–013), native hardware RDMA through
mlx5_2, direct-I/O files storage, `.efs` enabled and packing/containers disabled.
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
ENOSPC's exact budget/resource cause on this build has not yet been established.

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
| Latest query ENOSPC / publication integration — W92 / W109 | Generation, aggregate-root, reserve and temporary-sort accounting can be tested with small catalogues and a private fixture budget. This is a proposed discriminator, not an already proved small reproduction. | Reproduce the real host-worker sequence: restore checkpoint, catch up, retain/release an old reader, checkpoint, mutate a fresh subtree, refresh and query it. Measure every generation and temporary file against each budget; compare isolated publication with retained-reader overlap. | First attribute the deployed ENOSPC result. Preserve the unchanged ORCD 16 GiB generation / 32 GiB aggregate limits and query deadlines. A private small-fixture tripwire must expose the same accounting error, rather than increase production limits to hide it. |
| Fork during ordinary direct-mode reads — W106 | The diagnostic fails after seven verified files, so failure does not require reading 10 million files. Its existing fixture is still large; a genuinely small-fixture reproduction is not yet proved. | Generalize `tests/live/efs_direct_read_fork.c` to a fresh small deterministic fixture. Compare eight read threads with and without the ninth thread that forks every 5 ms; preserve exact mismatch bytes. Use a Linux FUSE client with the same read mode. | Application fork/read safety remains OPEN. Removing forks from the capacity oracle fixes its methodology, not application compatibility. Other Linux clusters may help debug; final hardware-RDMA acceptance still belongs on ORCD. |
| Snapshot/log-rotation foreground stalls — W97 / D30, with W109 contention | Log rewrite cost depends on retained command bytes, not namespace file count. Ordinary xattr churn can reach the normal snapshot trigger with very few files. | `tests/live/efs_raft_rotation_latency.c` builds a fresh isolated retained-log fixture and verifies every record after replay. `tests/live/efs_snapshot_pressure.py` drives normal 512 MiB command thresholds without changing them; pair it with `efs_foreground_latency.py`. | Buffered/cached-CRC rotation improves isolated timings but does not close the 5.569/22.044-second original tails. Live rotation still reached roughly 3.6 seconds for a 1.52 GB retained image, blocking snapshot advancement. Investigate lock ownership, storage barriers and export/GC interference before changing scheduling. |
| Long orderly startup — W92 / W97 scale investigation | A size ladder and phase profiling can separate metadata replay/index work from physical fragment enumeration. A faster/smaller machine could otherwise hide the cost. | Profile normal startup with increasing independent inode, chunk-record and fragment-file populations. Capture phase durations, CPU, faults, I/O and resource peaks; keep acknowledged-data checks and the 300-second gate. | The observed 284.704-second readiness is PASS with little margin. Cause and a small reproduction are unproved; it is a performance investigation, not a new correctness failure. Full-size orderly restart remains an ORCD gate. |

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
