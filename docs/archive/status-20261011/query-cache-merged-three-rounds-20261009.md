# W92 — merged-source suites and three further tiny-file rounds

**Archived Oct 11, 2026:** dated profiling and integration evidence. These rounds do not close W92 or establish the current Intel deployment. The [active W92 acceptance ledger](../../status/query-cache-closure-20261010.md) retains unresolved scale and platform gates.
Oct 9 EDT / Oct 10 UTC, `devel-find-stats`, Xorinox files/direct-I/O/TCP.
Baseline `8aeb9c4b` includes the devel, W66 and persistence-pressure merges.
The user explicitly grants exclusive Xorinox access. No wipe or push.

## Merged-source suite pass

The complete native Linux `make test` passes once before profiling, along with
all five architecture gates and POSIX harness self-tests. Live suites report:

| Suite | Result |
|---|---|
| Core POSIX vs local filesystem baseline | 212/212 each; equivalent |
| Two-client concurrent POSIX | 64/64 |
| POSIX extras, commands explicitly enabled on diagnostic mount | 4 pass, 0 fail, 1 unsupported shared writable mmap skip |
| Persistence across full daemon restart and fresh FUSE mounts | 26/26 prepare; 26/26 verify |
| Supplementary macOS NFS `cp -X` | 20/20 warm copies; exact bytes/inode identities independently verified through fresh gateway after restart |

The NFS evidence is narrower than the original W55 gate: macOS sudo requires a
password for client remount; plain cp and a cold macOS NFS remount are not claimed.
These tests do not close W55. No sanitizer or hardware-RDMA acceptance is claimed.
Each subsequent cache fix passes the focused production-code fault regression,
RAM/disk differential test (default and 12000 files), bounded-memory admission
and generation-quota refusal. This does not claim a second complete native pass.

## Three profiles and logical fixes

All three storage daemons have one 60-second 99 Hz CPU capture per round, nine
captures total, all exit 0 with zero lost samples. The original tiny_files.py
creates 6000 files with four workers, 1..512-byte payloads and up to three directory
levels. Retained namespace growth and random directory placement prevent an
isolated live throughput comparison. Profiles include idle tail after creation.
No compiler or frozen benchmark overlaps the CPU captures.

| Round | Profiled build | Seconds / files per second | New tree directories / bytes | Follow-up fix |
|---|---|---|---|---|
| 1 | `8aeb9c4b` | 45.8 / 131 | 185 / 1537520 | `4b24fba3`: exact raw-key search returns its validated slot immediately |
| 2 | `4b24fba3` | 46.7 / 128 | 191 / 1529095 | `a10583aa`: batch up to eight sequential derived inode rows per write |
| 3 | `a10583aa` | 47.2 / 127 | 223 / 1539127 | `a8a15a14`: four fixed LRU read windows per descriptor |

On xefs1 the query builder occupies roughly half the sampled CPU; window reads,
raw lookup and small writes recur in all three profiles. The last fix is measured
in the controlled benchmark and cold functional gates, not a fourth live profile.
Writes remain synchronous/write-through. Every overlapping cached window is
updated after successful writes; partial write failure invalidates all windows.
FD reuse clears them all. Short EOF blocks refill when later writes extend them,
including through sparse gaps. Candidate row batches flush before derived reads;
all existing publication persistence barriers remain.

The fault regression checks exact/missing/prefix lower bounds, value capacity,
batch tails and partial ENOSPC, four hot disjoint read blocks, overlapping writes,
EOF growth, descriptor reuse and raw/index/name/derived/manifest fsync refusal.
Failed candidates return no view; the previously published checkpoint restores.

## Controlled comparison

Each variant uses the same frozen 25425-file / 827-directory / 6234928-byte KV
copy and merged supporting library. Counts cover one fresh build and three
unchanged refreshes. Traced timings are not speed measurements.

| Variant | pread64 | pwrite64 | fsync | Accounted candidate bytes |
|---|---:|---:|---:|---:|
| Merged baseline | 1295903 | 381549 | 5363 | 2103112 |
| Exact raw-key lookup | 1276947 | 381549 | 5363 | 2103112 |
| Sequential row batching | 1276947 | 291549 | 5363 | 2107784 |
| Four read windows | 285379 | 291549 | 5363 | 2897032 |

Combined preads decrease 78.0%, pwrites 23.6%; fsync count is unchanged.
The fixed window allocation grows from 256 KiB to 1 MiB (32 descriptors × four
8 KiB windows); complete accounted scratch remains below the tested 4 MiB budget.
This increases fixed memory, not memory per indexed entry.

Three alternating untraced trials per baseline/final variant produce:

| Metric | Baseline median (range) | Final median (range) |
|---|---:|---:|
| Fresh build seconds | 2.781 (2.687–2.830) | 2.704 (2.661–2.794) |
| Unchanged refresh seconds, nine observations each | 0.900 (0.887–1.004) | 0.742 (0.715–1.146) |
| Four-build process CPU seconds | 2.241 (2.196–2.242) | 1.589 (1.572–1.651) |
| Four-build process wall seconds | 5.802 (5.690–5.899) | 5.201 (5.171–5.660) |
| Process peak RSS KiB | 14364 (14364–14448) | 15996 (15876–16036) |

Median CPU drops 29.1%; median refresh drops 17.6%, with overlapping ranges and
one slower final refresh retained in the evidence. Fresh wall ranges overlap.
These are warm filesystem-cache measurements on VMs, not cold physical-media
acceptance or evidence of a foreground creation-rate speedup.

## Deployment and remaining work

Final runtime is `a8a15a14` on all three storage nodes; matching source hashes,
wire build IDs, direct-I/O arguments and TCP environment are recorded. After cold
restart/fresh diagnostic mount, all eight retained tiny trees pass full exported
path sets and totals: 60000 files and 526 sampled payload hashes. Root snapshot
reports 62100 files, 2174 directories and 177570052 logical bytes, with refresh
error 0; extra files include the retained suite fixtures. The diagnostic mount
is drained/removed. Gateway and both test-client mounts remain available with
`.efs` disabled. No retained data is deleted.

W92 remains open: full derived-stat/rollup passes, aggregate retained-cache
capacity control and billion-file scale/fault acceptance. W66 authoritative
consensus/routing and W99 proactive all-writer pressure remain separate work.
The [raw ledger](../../../results/measure/20261009-w92-merged-three-rounds/README.md)
contains complete results, method scripts, original failure/retry logs and hashes
of retained remote captures. [Status index](../../status/README.md) · [W92](../../backlog/work-items.md#w92).
