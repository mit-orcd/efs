# AMD W73/W64 RXE acceptance — October 8, 2026 New York

Execution dates in raw logs use October 9 UTC. User scope: continue on AMD
only, restart/deploy as necessary and finish the RXE acceptance work. No NUC
or Xorinox operation belongs to this continuation.

## Source and host

Branch `devel-gate-closure` was cleanly fast-forwarded from `8d376739` to
integrated devel `f6233987b9d1243d5dc95167e9aaabc77e32b44d`, which already
contains the close-cleanup, truncate-cursor and append-handoff repairs. This
also retains accepted W58/W66/pressure changes instead of deploying the older
isolated branch over them. The main checkout was not edited.

AMD initially ran build `10cbfaf8da91`, direct I/O/TCP, four daemons and two
mounts with `.efs` switches disabled. Soft-RoCE `rxe0` is ACTIVE on `wlp194s0`;
GID index 1 is IPv4-mapped to 10.0.0.206. Memory locking is unlimited for the
service account. At preflight there is about 430 GiB disk space and 116 GiB
available RAM. This is one physical host and a software verbs device, not
hardware RoCE, native IB or a multi-host test.

The clean candidate is built at `/data1/efs/gate-rxe-f6233987`, with build ID
`f6233987b9d1`, branch `devel-gate-closure`. A first build command named the
nonexistent `tests/test_rdma` target; the corrected target is
`tests/test_rdma_xprt`. The rejected command log is retained separately.

## Isolation and gates

Runner `/data1/efs/amd-w73-w64-rxe.sh` owns
`/data1/efs/w73-w64-rxe.lock`. It gracefully drains the installed two mounts
and four daemons, borrows only AMD's policy-allowed `/data1/efs/mnt` and
`mnt2`, and creates unique private stores under `/data1/efs`. Installed n1–n4
stores remain intact. The exit path restores installed direct/TCP service;
after all gates pass, the same accepted candidate binaries are deployed with
strict RXE.

The transport probe passes 67 RDMA frames and one intentional oversized TCP
side-channel frame. SKIP is treated as failure. Live gates require verbs
file descriptors on every private daemon/client, including cold restarts;
transport proof is retained in each fixture's `rdma-runtime.jsonl`.

Direct and buffered RXE each run mount-wide sync acceptance, then full
POSIX/peer, O_SYNC/O_DSYNC before-close and failure/recovery, append sync
boundaries, durable replay/32-identical-request races, leader death, dirty-stop
recovery, shutdown mutations, mixed append/GC and exact 800 cold records.
Regression failures remain failures even if later independent gates pass.

## Results

| Mode | Core POSIX | POSIX wall time | Peer | Peer wall time | Mount-sync | Cold append/GC |
| --- | --- | --- | --- | --- | --- | --- |
| Direct / RXE | 212/212; zero failures/skips | 19.0 s | 64/64 | 14.4 s | PASS | 800 exact records; PASS |
| Buffered / RXE | 212/212; zero failures/skips | 18.6 s | 64/64 | 14.2 s | PASS | 800 exact records; PASS |

Both full runs pass all requested sync/recovery/replay gates and final physical
restart reclamation. The shared runner exits zero after restoring installed
service. There were no new production-code changes required in this RXE round.

The four manifests and installed verification agree on production-source SHA256
`10b96918bab7eee98d5f39d9cd09adb4d6473b1522928a24679745d29a835c5c`.
Installed `efsd`, `efs-fuse` and `efs-mgmt` have exactly the binary hashes used
by all four gates; deployment does not rebuild them. Each mount-sync fixture
retains three runtime verbs snapshots and each full fixture fourteen, with
transport-up log excerpts alongside them.

The integrated POSIX runner now defaults to 212 core tests. Optional mmap and
`.efs` capability checks were moved to `posix_extras.py`; this count change
must not be confused with the earlier NUC 213 passed / four skipped all-suite
result. Feature commands remain explicitly disabled for these gates.

## Remaining scope

W64 production mount-session fencing, stream reclamation and unknown-outcome
acceptance depend on W72 and remain open. No D25 activation or hardware
power-loss claim (W71) follows from these process-crash gates. Hardware/native
IB and physical multi-host tests remain distinct from software RXE acceptance.


## Installed AMD service

`/data1/efs/src` now contains accepted integrated source `f6233987b9d1` on
`devel-gate-closure`, plus all five freshly built binaries. The prior complete
source/binary installation is preserved under `deploy-backups/`; the exact
path is in the raw deployment evidence. Installed stores were not formatted
or replaced. Deployment gracefully drained mounts and daemons and exits zero.

Four daemons and both mounts are verified running with strict
`EFS_TRANSPORT=rdma`, `EFS_RDMA_DEV=rxe0`, `EFS_RDMA_GID_INDEX=1`, direct storage
I/O and both `.efs` switches disabled. Every process has a verbs device handle.
Peer-before-close O_DSYNC ordinary and append writes pass, followed by peer
truncate and exact subsequent append. The unique smoke tree was removed.

The separate installed optional suite passes `mmap_private_not_visible`;
shared mmap is unsupported and three `.efs` checks skip because commands are
explicitly disabled: **1 pass / 0 fail / 4 skip**. The core counts above omit
these optional checks by design, rather than hiding core failures.

## Evidence and closure boundary

[Retained raw evidence](../../results/measure/20261009-amd-w73-w64-rxe/README.md)
contains full runner/build logs, manifests, verdicts, core/peer/optional TSVs,
verbs snapshots, transport excerpts, deployment commands and installed checks.
Full private daemon/client logs and stores remain on AMD at the fixture paths
recorded alongside each manifest. No main checkout merge or push was performed
by this continuation. NUC and Xorinox were not operated.

W73's supported software TCP/RXE × direct/buffered matrix and mount-sync
acceptance are complete, combining the earlier NUC TCP evidence with these
AMD RXE cells. W64's targeted replay/append gates pass over RXE too, but W72
production session fencing/unknown-outcome work remains a real implementation
dependency. This does not close hardware, physical multi-host or W71 power-loss
acceptance.
