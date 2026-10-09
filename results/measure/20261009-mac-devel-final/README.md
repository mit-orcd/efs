# Mac devel integration, 2026-10-09

Tested source: `43fcefe190dd` on `devel` (full SHA in source-commit.txt). All 15 branch tips captured in branch-tips.txt are ancestors of this source; branch-coverage.txt records the check. Integration combines the metadata-scaling/W98 work with subsequent W66 session fences, W92 disk quota and clean-cluster query validation, and bounded-inbox regression coverage. Generated architecture documents were regenerated after merging; both competing unit-test additions were preserved in the Makefile.

## Environment

Mac-hosted Tart Fedora/aarch64 VMs: storage efs1–efs3, gateway efsgw. TCP transport, buffered storage, existing /data1 files preserved. Native baselines used XFS (/dev/vda3) on the gateway. Every EFS daemon, client, build and workload ran under Linux user `efs`; administrative provisioning used sudo. Pressure activation was not enabled for these gates. No live forced-discard, forced unmount, filesystem wipe or data reset was used.

The complete build and unit suite ran in /var/tmp/efs-devel-43fcefe190dd on efs1 before installation. Runtime hashes matched on all four VMs (install.log and installed-*.txt). Both efsd and efs-fuse identify the tested devel source. The earlier snapshot's interrupted live run is retained in ../20261009-mac-devel/ and is superseded.

## Completed gates

| Gate | Result |
|---|---|
| Full make test | PASS |
| Architecture | PASS, all 5 checks |
| XFS POSIX baseline | 212/212 PASS |
| XFS two-client baseline | 64/64 PASS |
| EFS POSIX original run | 139 PASS, 14 FAIL, 59 NOTRUN (212 total) |
| Previously unrun POSIX cases | 56 PASS, 3 FAIL, 0 NOTRUN (59 total) |
| Combined completed core attempts | 195 PASS, 17 FAIL, 0 NOTRUN (212 total) |
| Isolated directory listing retry | FAIL, timeout at 30s |
| Two originally cancelled concurrency cases, isolated retries | Both FAIL, timeout at 30s |
| EFS two-client bounded remote runner | 59 PASS, 5 FAIL (64 total) |
| Two-client timestamp local retry | 1/1 PASS |
| Persistence prepare | 25 PASS, 1 FAIL (26 total) |
| Persistence verify after clean remount | 25 PASS, 1 FAIL (26 total) |

The core POSIX run used four jobs and a 30s per-test deadline, retaining each case's own larger budget (e.g. the 180s large-file truncate cases). The original suite stopped queuing tests when a timed-out worker could not immediately be reaped. remaining.py selects exactly its NOTRUN cases and uses the unchanged runner, test bodies, concurrency and deadlines. Follow-up logs preserve additional isolated attempts separately rather than replacing the original results.

Two independent FUSE mounts on efsgw were exercised using the existing remote runner and its standard 45s per-step deadline. two-mount-exec.py routes each side to its own mount and bounds only test subprocesses. Four workload cases exceeded that deadline: shared pwrite, concurrent append, append versus truncate, and create/unlink/stat churn. The fifth failure, peer_dir_mtime_bump_visible, is a remote-runner harness error (KeyError: m): its closure state is lost between separate step processes. The same check passed with the local runner. All raw outcomes remain in posix2.tsv and timestamp-local.tsv.

The resumed POSIX run found mtime_monotonic_many_writes timing out, and both 300 MiB large-file truncate setup writes returning BlockingIOError/EAGAIN. Client log excerpts in followup-progress.txt show data-cache body pressure during this interval. These are real unsuccessful writes, distinct from deadline-only failures; this run does not establish whether integration introduced them.

Persistence's many_files_in_one_dir prepares 500 files. It exceeded its standard 60s preparation deadline after 69 files. Verification found those 69 entries, so the incomplete preparation accounts for the missing intended entries; this is not evidence that a successful full preparation lost files. The other 25 persistence cases verified after an ordinary drain/unmount/remount.

The two cases cancelled during the first suite abort (concurrent_create_unlink_two_proc and concurrent_write_and_readdir) each timed out on their own isolated retry. combined-posix.tsv joins the original results with the NOTRUN and cancellation follow-ups: 195 passed, 15 exceeded deadlines, and two returned EAGAIN; no case remains unrun. The isolated directory listing retry also timed out at the same 30s deadline with one worker, so that failure is not explained solely by four-job contention. Original failures are retained, not replaced with later outcomes.

## Restart handling

Normal gateway stop initially found the FUSE mount busy. Diagnostics identified the NFS kernel export cache holding the mount; withdrawing the export and flushing that cache allowed a normal stop. A client-retirement race reported an already-exiting daemon; verifying its exit and retrying the normal stop succeeded. See mount-holders.txt, nfs-cache-drain.log, stop-retire-race.log and stop-before-deploy.log. Storage daemons were stopped only after clients had drained. Persistence also used export-cache withdrawal before its normal remount.

This is not an all-green integration result. Raw logs, TSVs, verdicts, helper scripts and installed build hashes are retained here.

## Final runtime state

All three storage nodes remained active with healing idle, and installed efsd/efs-fuse hashes still matched the tested source on all four VMs. Processes remained owned by `efs`. No test workers remained after the follow-ups.

A post-test ordinary gateway drain/remount was attempted. The client refused the drain because unresolved writes or lease releases remained (final-refresh.log and final-state.log). The mount and client were retained and the NFS export restored. No forced discard was attempted. This is an additional unresolved recovery/drain finding after the failed large-file tests; the cluster is left running on the tested build rather than claiming a successful post-test refresh.
