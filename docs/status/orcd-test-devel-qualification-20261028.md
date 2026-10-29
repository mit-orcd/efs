# ORCD-test merged devel qualification — October 28, 2026

**FAIL overall; cluster left running.** User-authorized merge, branch deletion,
deployment and full POSIX/IOR rerun. Tested source is committed devel
`714d7e6fc8766798fcaeaca373c337750b3f3368`, the merge of the remaining
[second profiling cycle](orcd-test-dd-round2-20261028.md) changes. The original
index fix was already integrated by wave 30. The merged copy-elision attempt
and its reversal cancel; no copy-elision product change is deployed.
`agent/orcd-dd-profile-20261028` was deleted after its commits became reachable
from devel. No push. Other sessions' uncommitted files were excluded and
preserved byte for byte, with a local generated-file backup.

The IOR exit-status and retired-replica readiness fixes are retained.
Integration keeps devel's profiling guard in the fast `make test` list, adds
the mocked IOR guard there, and therefore needs neither long-test budget row.
Focused recipe/IOR/readiness checks, nine graph tests and five architecture
checks passed on the integrated tree. No fresh full `make test-all` was run
for this deployment; previous full-gate failures remain in the linked report.

## Deployment and scope

Four storage nodes fcstor003–006, six NVMe roots per node, seven clients
fcstor007–013; `/tmp/efs-mount`. RHEL 9.8, kernel 5.14.0-687.39.1.el9_8,
EPYC 7402P / 24 visible CPUs / 251 GiB per host. Private libfuse 3.16.2,
native RDMA mlx5_2, O_DIRECT fragment I/O and buffered FUSE writes.
Automatic safe-headroom quotas, no override or policy change. Existing
commands/packing policy kept. No fstor007 or other cluster touched.

Eleven exact committed-source builds passed (`make -j8 all`) before any
service change. Every old client drained, every old daemon stopped cleanly.
Existing stores were retained without formatting, with old sources preserved
at `/tmp/efs-before-devel-714d7e6fc876`. Stable leaders, all current voters,
applied root and a fresh root read passed readiness before all seven mounts.
Ordinary elections choose placement; no diagnostic transfer was requested.

A relative controller path caused an initial preflight log-copy failure,
before any build/stop/deployment. It is retained separately. The corrected
controller resolves its evidence root before contacting compute hosts; no
failed data test was retried in place.

## Test matrix

| Suite / profile | Result |
| --- | --- |
| Full POSIX, serial, fcstor007 | 221/223 PASS, 0 FAIL, 2 SKIP |
| Full POSIX, all seven clients together, 16 jobs/client | Each 220/223 PASS, 0 FAIL, 3 SKIP |
| Persistence prepare and verify across clean full cold restart | Each phase 27/27 on each client: 189/189; PASS |
| Local two-client, two independent mounts | 68/68 PASS |
| Mounted live suites | 12/14 PASS; concurrent EOF extend and fork control FAIL |
| Real-host two-client pairs 007/008, 009/010, 011/012 | Each 66/68 PASS; shared-pwrite and concurrent-append steps timed out |
| Isolated expiry lifecycle on fcstor007 | PASS |
| Full isolated expiry lifecycle plus POSIX core on fcstor013 | PASS; core 218/218, 0 FAIL/SKIP/NOTRUN |
| 28 concurrent full POSIX instances, four/client, 16 jobs/instance | 5,658 PASS, 488 FAIL, 49 SKIP, 49 NOTRUN; all 28 instances FAIL |
| IOR, single-client and 56-rank/7-client easy/hard | All four PASS with readback checking |

The serial skips are unsupported mmap (`ENODEV`) and find-query indexing
outside the short case budget. Seven-client runs also skip the short-budget
stats query. Dedicated query gates separately pass; skips are not converted
to PASS. The ten virtual-interface checks pass in 903 s, including resource
recovery, pinned snapshot bytes, mutation statistics and authorization; the
committed wrapper uses a 150 s freshness bound. The catalog reports a
60,000 ms refresh interval. Large-delta create/update/delete/rename/hard-link
checks and the exact peer oracle pass on 1,025 files. Concurrent command load
passes with **120,548/120,548 payloads read and hash-checked from the peer**;
publication convergence after writers stop is 54.70 s. This coverage is for
that generated fixture, not all existing files.

| IOR case | Ranks / clients | Write MiB/s | Read MiB/s |
| --- | ---: | ---: | ---: |
| little easy | 1 / 1 | 335.69 | 1528.34 |
| little hard | 1 / 1 | 37.99 | 155.19 |
| wide easy | 56 / 7 | 457.65 | 15990 |
| wide hard | 56 / 7 | 45.10 | 532.94 |

All four return zero with the repaired committed runner. They ran without
an overlapping regression workload. These are not a controlled A/B against
the previous build; no causal speedup claim or 1 GB/s claim follows.

## Failures, not waived

- **Standalone concurrent EOF extension, no fork:** 159/160 pass. Case 70
  reports `extend: [Errno 5] Input/output error`, inode 53179, size 1,048,577
  instead of the successful-run target 8,388,609. The oracle combines errors
  from open/pwrite/close and does not identify the failing syscall/thread.
  This is a failure to complete the workload, not proof of acknowledged data
  loss or a proven kernel issue. Fixture
  `/tmp/efs-mount/extend-eof-9ygvn5sv/70/f` is retained. Root cause OPEN.
- **Fork exclusive-create control:** 153/160 pass. Seven first-read wrong-byte
  observations (`00` or `01` instead of `78`) have the W121-style signature;
  their diagnostic payload readbacks show `78`. The original observations
  remain failures; later readback never reverses them. There is no independent
  FUSE-reply trace in this run, so kernel attribution is not newly proved.
  Fixture `/tmp/efs-mount/exclusive-visibility-xyk9k3km` remains.
- **All three remote two-client pairs:** `peer_shared_pwrite` and
  `peer_concurrent_append` return 124 at step 1 after 15 s, with empty stdout.
  The deadline covers the entire SSH-launched step, including 500 pwrite/fsync
  loops per side or 400 appends per side; it is not merely connect timeout.
  The later integrity-check step was not reached. Local two-mount runs pass,
  but have different topology/load and do not erase these six failures.
  Transport versus operation latency is not isolated; deadlines were not
  raised and cases were not retried. Their unique parent trees remain.
- **Stress:** 488 failed rows include 204 explicit EIO reports, deadlines,
  child exits/signals and query subprocess failures. All 28 instances fail;
  this is not classified wholesale as W121. Frequent affected tests include
  deep/crazy names, concurrent creates/readdir, honest truncate, disjoint
  writes, extension and mtime. fcstor008 stress-0 aborts at 32.1 s because a
  timed-out worker is still alive at cleanup; its remaining 49 cases are
  NOTRUN, not PASS. Its retained root is
  `/tmp/efs-mount/posix-fcstor008-devel-20261029T003722Z-stress-0-upvtdfmy`.
  A later corrected process census finds no task workers on any client.

The stress wrapper's display command `tail -4` also errors when given multiple
logs on this host; use `tail -n 4` when reproducing the display. All 28 canonical
TSVs independently contain failures, so that wrapper defect is neither their
cause nor the basis of the reported counts. The first worker census included
its own probe process; those records remain, alongside corrected per-host
empty worker lists. No failed case was rerun or its result replaced.

Every case has a fresh tag and retained command, deadline, return code and log.
`--suite all` covers core and optional extras. Existing per-test budgets and
oracles are unchanged; wrappers bound whole jobs and do not turn timeout into
PASS. Expiry is tested in an owned temporary loopback fixture; it never changes
the public cluster policy. The live wrapper's zero exit alone is not a verdict:
every canonical result row, including the W121 fork control, is reported.
Persistence checks include size-only cases; they do not verify all retained
cluster payloads, and a clean restart is not a power-loss qualification.
IOR read rates are not cold-device measurements.

## Reproduction

Deploy only the pinned committed snapshot with the existing ORCD driver;
require successful builds on all eleven hosts, checked stops, a single matching
build, readiness and seven mounted clients. SSH agent forwarding is required
on both login and compute hops; never use BatchMode. From `/tmp/efs`:

```sh
python3 tests/posix/posix_suite.py /tmp/efs-mount --suite all --jobs 1 --tag UNIQUE-serial --results SERIAL.tsv
# On each of seven clients concurrently:
python3 tests/posix/posix_suite.py /tmp/efs-mount --suite all --jobs 16 --tag UNIQUE-parallel --results PARALLEL.tsv
python3 tests/posix/posix_persist.py /tmp/efs-mount --tag UNIQUE --phase prepare --results PREPARE.tsv
# Checked stop of all seven mounts and four daemons; restart, readiness, remount:
python3 tests/posix/posix_persist.py /tmp/efs-mount --tag UNIQUE --phase verify --results VERIFY.tsv
bash scripts/orcd_test_ior.sh NEW_IOR_OUTPUT /tmp/efs-mount
# Separate second mount on fcstor007:
bash scripts/orcd_test_live_suites.sh NEW_LIVE_OUTPUT /tmp/efs-mount /tmp/efs-mount-devel-peer
# Remote two-client: prepare a unique parent, then --remote on pairs
# 007/008, 009/010 and 011/012, with the explicit deployed script on both sides.
python3 tests/posix/posix_expiry.py --private
# Stress: four independent --suite all --jobs 16 processes per client,
# distinct --tag/--results for each, all 28 running concurrently.
```

IOR uses fsync-on-close/read checking: single 8 GiB easy (1 MiB transfers),
single ~0.94 GB hard (47,008-byte transfers), 56-rank/7-host easy (2 GiB per
rank) and hard (~5 GB shared). Whole-case deadline 3,600 s. Serial/parallel,
persistence and remote-two-client jobs have 3,600 s limits; live wrapper
22,000 s, private expiry 7,200 s, stress 5,400 s. No existing test deadline was
relaxed. The detailed controller/individual commands are retained externally.

## Evidence and limits

Raw evidence stays on ORCD, not in Git or copied to the Mac:
`/home/erbmi1/orcd/scratch/efs/orcd-test/devel-qualification-20261029T003722Z-714d7e6fc876/`
(resolved path `/orcd/scratch/orcd/001/erbmi1/efs/orcd-test/` with the same final
component). UTC directory date is October 29; the user-facing run date is
October 28 in America/New_York. Failed preflight:
`devel-qualification-20261029T003656Z-714d7e6fc876/` under the same parent.
Source archive SHA-256:
`f89a5471833640f60c3eb649635b2cd464d8c1f67e74d52eceab77446ff4c779`.

Final running-executable hashes match installed source on all four servers
and seven clients; all seven mounts and post-stress readiness pass. The extra
two-client mount was cleanly removed; owned expiry daemons/mounts stopped.
The cluster is left running on `714d7e6fc` with retained stores. All seven
mounts also pass a post-stress read of the same immutable, nonzero 256-byte
canary from the verified command-load fixture. This is a smoke check, not a
full post-stress payload audit. Separate closed records are under the same
parent in `devel-qualification-20261029T003722Z-714d7e6fc876-postchecks/`;
its verified manifest digest is
`114fad31b18057a4b31c84e8773505318e231751dc53ab0c40afa9aa6adcc607`. The campaign
controller's exit 0 means orchestration completed, **not a passing campaign**.

`results-summary.json`, `stress-summary.json`, all TSVs/logs, source/build
proofs, readiness and `health/` are retained. The main verified `SHA256SUMS`
covers **441 files**, digest:
`7a50fdeaa92b7c90aeb1a06598c638d4b5ab857bf2d329e80733464b693408b1`.
The failed preflight's verified manifest covers 12 files, digest:
`61a57f58c4a0b80ae6b3589df74a108790868d4469653e6079ce74d2dcf2fcca`.

`PRUNED.txt` records removal of the reproducible source archive. The passed
full-expiry temporary stores were pruned only after checking process/mount
absence and copying/hash-verifying 13 closed reports; exact paths and hashes
are in `fcstor013/expiry-full-prune.json`. Empty misdirected preflight copies
were preserved under the failed-attempt directory and removed from home.
Existing stores, prior failed evidence, the standalone failed fixtures, remote
parents, stuck stress root and public query/load fixtures remain for the open
failure/evolving-store throughput investigations. Their full cold verification
is not claimed. **Retention limit:** the committed core POSIX runner normally
cleans completed test trees even after ordinary failures. Consequently not
every individual failed stress payload survives; logs/TSVs are sealed, but
complete fixture replay is not claimed. No broad public-namespace cleanup,
reformat, push, hidden deadline relaxation or product fix occurred in this run.

The next repair work should isolate the no-fork EIO and loaded EIO/deadline
failures, with retained fixtures and traces, before claiming stress acceptance.
Prior quota, sanitizer and platform/kernel gates are not closed by this run.
