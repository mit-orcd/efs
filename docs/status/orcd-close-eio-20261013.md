# ORCD tiny-file close EIO investigation

Status: **finite close/fsync gate PASS; sustained tiny-file load FAIL**.
All seven original benchmarks later fail close EIO after about29minutes.
The140000-file finite pass does not close sustained-load correctness.
The user requests restarting all seven on the same installed correction;
failed fixtures/logs remain preserved.10million-file targets are not complete. Failure baseline was
fc341f00e3ac2a8decddd408ef47aeb2c7e02e34. Current installed correction is
e8bab21014bad21e5ea2198141e4df3a45d3dc3d, verified on all11 hosts.
Gate: normal-operation close/fsync publication, preserving W17.1 deadlines.
Four servers/seven clients, native mlx5_2 RDMA, files/direct I/O, `.efs` enabled;
packing, containers and expiry disabled. All seven tiny-file runs failed with
EIO during close after39–146s, using16workers each and10million-file targets.
Logs and failed payloads are preserved in ORCD scratch, per-host
`tiny-files-fc341-20261013-210651.log`; local identity/progress summaries are
in operations/devel-efs outside Git. No old failures are overwritten.

Client evidence proves close metadata publication exhausts the8s sync REPORT
budget, returns EFS_ERR_BUSY(-13), and FUSE flush maps that to EIO. Example007:
21:08:37.416Z inode21359379, REPORT8018ms, one chunk. Server REPORT activation
waits also return BUSY/NO_QUORUM after multi-second waits. The deployment already
contains04f4c8c admission progress and1b3844d explicit-zero EOF corrections.
They have not resolved this workload. Newer devel-rdma transport/read-control
commits are not deployed and do not close this observed failure.

Working theory: maintenance work bounds count live callback rows but not hidden
tombstones. LSM merge scans hold the KV mutex, so tombstone-heavy close/orphan
pages can block Raft apply and authority progress. Actual004/005 maintenance
logs show4–8s passes traversing about578000–637000merged keys, overwhelmingly
tombstones, alongside stalled REPORTs. This is a demonstrated unbounded work
path and a plausible contributor; logs alone do not prove it is the solecause.

The correction adds a bounded maintenance scan with4096merged-key pages, including
tombstones, and continuation from the last consumed key. Close-step, close
cleanup and orphan/discovery use it. Existing live-row callback limits,
validation, proposals, syncREPORT8s and receiptwindow16 stay intact. Full scans
and immutable snapshot scans are unchanged. A work page does not imply range
exhaustion or authorize deletion. This bounds keys, not elapsed time or initial
segment opening; further blockers remain possible.

Regression: tests/test_kv_lsm.c seeds10000rows, flushes, deletes9990, flushes and
reopens.17key work pages must bound observed keys, advance across tombstone-only
pages, visit all10survivors exactlyonce and terminate. Callback refusal must
retain its original outcome without a fabricated work continuation. Reproduce:
`make -j4 tests/test_kv_lsm && EFS_TEST_KV_WORK_ONLY=1 ./tests/test_kv_lsm`, then
full native tests and sanitizer qualification. Linux full native `make -j4 test` PASS on exacte8 source. Full KV suite and
work-page regression PASS under ASan/UBSan with leaks enabled; disabling only
the key-work stop makes the new bound oracle FAIL as intended. The actual
close-GC embedded source test also PASSes normal and ASan/UBSan, including
tombstone-only continuation. Local full build is blocked by existing Linux-only
fopencookie logging, so qualification used native Linux. An initial native run
on1a2234a1 stopped at the old embedded close harness; e8bab210 corrects that
harness and adds a cursor-capacity guard. Final full native is PASS.

Reproduction uses a frozen source and normal drain/stop/install/start/remount
without erasing stores. Repeat the same seven-screen workload in
fresh uniquely named directories, retain all failed data, and verify actual
close results and peer payloads. Do not claimfixed from a short quiet pass or
change deadlines to conceal failure. General `.efs` scale acceptance remains
onhold; this user-authorized close EIO repair is active.

## Installed correction and loaded qualification

All866 committed code/test inputs match on all11 hosts. Exacte8 full native
qualification on fcstor006 PASS; efsd SHA256
`6468e6c0922f7682479d68cbebf39fc44c7be53914707a2b987962c3c03ee24c`,
efs-fuse `e546cab015165a960c13892f3aade87c8d6975e8e9bb95b99cdf7bdc9b1c7177`.
Native flags/profile are the same as the failure baseline; no storage erasure,
timeout widening, receipt-window change or switch to segmented RDMA. All seven
clients drained normally; four servers stopped normally; startup/remount and
acknowledged large/512-byte markers PASS on all7. Runtime verifies nonroot UID,
RDMA device FDs, direct-I/O/files, commands enabled and disabled feature flags.

Simultaneously on fcstor007–013:

```sh
cd /tmp/efs
python3 -u tests/live/fsync_admission.py /tmp/efs-mount --files 20000 --workers 16
```

**140000/140000 PASS**, no failures: create, write, fsync, close, exact EOF/payload
read-back and unlink. Each host completes in212.580–214.213s, retaining failed
fixtures if any (none); successful owned fixtures clean themselves. Raw output
is per-host ORCD scratch `close-eio-20000-e8bab21014ba.log`; aggregate local
`operations/devel-efs/close-eio-seven-fsync-e8bab21014ba.json` is outside Git.

The original `/home/erbmi1/bin/tiny_files.py` SHA256 is
`3b05d2f8f5e08c16a6b6a04f3b79bef1b2763326f9e555d5f681c31547d3f850`.
Seven existing screens run it again with depth8, max1000files/folder,
max10000000files and16workers, in fresh per-host directories prefixed
`tiny-files-fcstorNNN-e8bab210-20261014-010141` (UTC filename stamp).
Previous failed directories and logs remain intact. At the recorded check:

| Client | Last reported files | Seconds | Files/s | Process |
| --- | ---: | ---: | ---: | --- |
| fcstor007 | 51625 | 287 | 180 | RUNNING |
| fcstor008 | 53585 | 287 | 187 | RUNNING |
| fcstor009 | 52081 | 287 | 181 | RUNNING |
| fcstor010 | 49718 | 283 | 176 | RUNNING |
| fcstor011 | 51391 | 283 | 182 | RUNNING |
| fcstor012 | 49527 | 283 | 175 | RUNNING |
| fcstor013 | 49961 | 278 | 180 | RUNNING |

No closeEIO in those progress logs. This exceeds the old39–146s failure window
and old per-host acknowledged counts, but is **not**70million-file completion
or a full payload audit of these running benchmarks. Finite correctness gates
supply the completed verdict; benchmarks remain running for the user.
Sampled new005 maintenance has390passes, max945ms total/max244ms orphan;
004 has242passes/max225ms total with no orphan work in its current role.
These are source/time/role-specific observations, not universal wall-time bounds
or a performance qualification. Initial baseline4–8s orphan phases and code
inspection plus negative control support the tombstone starvation theory;
the controlled loaded pass establishes the targeted correction, not closure
of all other possible REPORT, capacity, recovery or EOF defects.

The correction is committed on devel-orcd-package1. Devel staysfc341f00 while
Intel's checkout has separate W100 work in progress; Intel explicitly requested
that it remain untouched. Newer W100/segmented-RDMA commits were reviewed as
separate objects and not mixed into this qualification. General `.efs` scale
acceptance, final400GiB audit and its open gates remain onhold.

## Sustained-load follow-up — failure remains open

A later process/log check finds all7 jobs stopped with EIO during close on the
same verifiede8bab210 deployment and profile. Last reported progress:

| Client | Files | Seconds | Files/s |
| --- | ---: | ---: | ---: |
| fcstor007 | 267638 | 1761.6 | 152 |
| fcstor008 | 278601 | 1760.6 | 158 |
| fcstor009 | 266434 | 1755.4 | 152 |
| fcstor010 | 264997 | 1754.8 | 151 |
| fcstor011 | 274464 | 1753.2 | 157 |
| fcstor012 | 266422 | 1754.3 | 152 |
| fcstor013 | 273791 | 1740.7 | 157 |

Therefore the scan-work defect is corrected and finite gates pass, but the
prior starvation theory is not a complete explanation or a sustained-load
closure. Correlating these later failures with fresh client/server logs is
still required; no later root cause is claimed. The user requested restart,
so the existing screens start fresh uniquely named benchmark directories,
with failed directories and logs retained. Local launch/progress identities
are archived before their current summary files are reused. Runtime and866
installed-input hashes are reverified on all11 hosts before restart. No daemon
restart, deployment, timeout/profile change, storage erasure or resume of an
invalid partial fixture is part of this benchmark restart.
