# ORCD tiny-file close EIO investigation

Status: candidate correction, mounted qualification NOTRUN. Current live source
is fc341f00e3ac2a8decddd408ef47aeb2c7e02e34, verified on all11 hosts.
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

Candidate adds a bounded maintenance scan with4096merged-key pages, including
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
full native tests and sanitizer qualification. Linux qualification pending;
local full build is blocked by existing Linux-only fopencookie logging.

After qualification, deploy a frozen candidate using normal drain/stop/install/
start/remount without erasing stores. Repeat the same seven-screen workload in
fresh uniquely named directories, retain all failed data, and verify actual
close results and peer payloads. Do not claimfixed from a short quiet pass or
change deadlines to conceal failure. General `.efs` scale acceptance remains
onhold; this user-authorized close EIO repair is active.
