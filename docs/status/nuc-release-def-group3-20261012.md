# NUC final common def candidate group3 — 2026-10-12

Exact source `def46109909c793e58ae2c453c5de537cdb2c627`, central frozen
`qualification-v030-tcp-direct`. Archive SHA256
`704cbfdc93db554f17248b9f2668d71de3799534e8d74f10a5a94d8b3b12264d`.
Full tracked manager manifest verified before normal build and before/after
live gates. Build `def46109-group3-normal`; binary hashes/version recorded.
This candidate differs from2bf only by reviewed native test adapter correction.
Earlier52/2bf results remain separate, not transferred into acceptance.

Scope TCP/direct I/O/FILES. Flow, early retirement, group drain and repair off.
No build, profiling or other daemon/loaders during mounted measurements.
Unique /data1-only clean stores, private ports24210–24213, policy mounts mnt
and mnt2. Existing helpers, assertions, case deadlines and cleanup unchanged.
Seven-client quota explicitly4G to match AMD/Intel; prior52 NUC1G diagnostic is
not a like-for-like hint comparison. No backwards compatibility or preservation
qualification. No source fixes, push, tag or promotion.

## Exact results

| Command/gate | Verdict | Duration |
|---|---|---:|
| `gc_reclamation.py --mode direct --transport tcp --posix` | PASS, exit0 | 97.540s |
| POSIX core within that fixture | 216PASS,0FAIL,0SKIP,0NOTRUN | 21.2s |
| Peer within that fixture | 67PASS,0FAIL,0SKIP | 12.3s |
| Seven-client mounted fixture, `--posix --posix-jobs 16 --fixture-quota 4G` | **FAIL**, exit1 | 170.051s |
| Peer after seven concurrent suites | 67PASS,0FAIL | 18.3s |
| Existing native `tests/test_store_gc`, after mounted measurements | PASS, exit0 | Raw log |

Complete seven-client suites each have221 cases:

| Client | PASS | FAIL | SKIP | Seconds |
|---|---:|---:|---:|---:|
| 0 | 219 | 1 | 1 | 95.4 |
| 1 | 219 | 1 | 1 | 93.6 |
| 2 | 219 | 1 | 1 | 94.7 |
| 3 | 219 | 1 | 1 | 95.3 |
| 4 | 219 | 1 | 1 | 91.2 |
| 5 | 218 | 2 | 1 | 94.3 |
| 6 | 219 | 1 | 1 | 94.6 |

Every client failed `dir_deep_nesting`: unchanged30s timeout. Client5 additionally
failed `virt_stats_readable`: unchanged30s timeout. Each skipped unsupported
mmap. No ENOSPC failures at4G. The hint correction does not eliminate the
seven-client deep-directory deadline on NUC. This establishes an affected
workload failure, not its root cause, drive latency ceiling, or W97 improvement.

## Physical and data correctness distinction

Core physical oracle passed actual closed-unlinked reclamation, failed-delete
retention, offline-member retry ledger/rejoin completion, deletion-driven GC
passes/removal counters, restart empty inventory/zeroquota and latePUT fencing.

Seven-client run also completed actual acknowledged cold hashes, physical
fragment reclamation and metadata queue drain **PASS**. Its final report
has `cold_verified=true` and seven POSIX client failure entries. Final assertion correctly returnedFAIL from those suites.
There is no global physical-inventory confound or client-stop failure in this
finaldef run. Do not report the seven-client run as overallPASS simply because
its physical/data oracles passed. Existing helper cold assertion is a correctness
check, not standalone recovery qualification.

Native GC passed actual two-root/format-switch, buffered/direct primitive,
quota recount, checksum, syscall failure and idempotence coverage; this is not
mounted buffered qualification. RDMA and repair remain deferred.
Full originalW52/both-modeW112 and sustainedW97comparison are not closed here.

## Evidence and final host state

Raw source/build/command/exit/timing evidence:
`results/measure/20261012-nuc-def-group3/controller/`.
Complete TSVs, manifests, GC JSON, client logs and fixture reports:
`results/measure/20261012-nuc-def-group3/fixtures/`. Collected archives retained.
Remote source/controller `/home/efs/nuc-def-group3`; identified test stores
`/data1/efs/evidence/nuc-def-group3`. Final matrix exit0 means controller queue completed;
individual seven-client result remainsFAIL, not aggregate acceptance.

All owned fixture daemons and mounts stopped after gates; no efsd remains,
policy paths resolve to underlying/data1. No automatic old-binary restoration
was requested for follow-on fixtures under the newer human instruction. Original
service was drained before quiet comparison; host services outside EFS untouched.
Manager received exact failures and quota context. NUC currently has no running
EFS cluster; subsequent deployment belongs to coordinated next action.

**Raw evidence note (2026-10-23):** the bulk data in `nuc-def-group3` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
