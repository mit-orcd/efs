# Xorinox W58 / W55 resumed validation

Branch `devel-known-bugs`, based on devel `2b9da49f`.

- `resumed/`: first resumed private fixture, xefs1 loopback ports 28420–23,
  four files-engine daemons, direct I/O/TCP. Source at `ddbdec1a`.
- `final/`: follow-up fixture on ports 28430–33, including ordinary CREATE
  replay-capacity refusal and fault-build-only actual FUSE reply-loss/BUSY gate.
  Its manifest hashes the tested source tree and binaries; rsynced source has no
  Git metadata and some binaries report `unknown`. Hashes are authoritative.
- `unit/`: captured Linux metadata red/green, CREATE retry/ownership checks,
  parallel PUT deadlines and publication durability tests.

The first resumed run ends `GC reclamation direct PASS`. Its `put-barrier-fault`
ledger records three refused publication cases and one successful two-fragment
quorum followed by cold exact-byte checks. The first file-barrier marker fails
all selected targets; partial fails nodes 1 and 2; directory fails all selected
fragment-directory syncs; quorum fails node 1 through restart.

The pressure red run restores only the prior receipt-capacity behavior. The new
regression then fails (with cascading namespace/replay assertions); green restores
receipt refusal and passes the entire metadata apply binary.

`final-focused.log` includes passed CREATE gates and then an invocation error:
`test-put-reply-deadline` is not a Make target. The intended test was subsequently
run directly as `python3 tests/test_put_reply_deadline.py`; `put-deadline.log`
records its pass. This command error is not a product test failure.

Native FUSE evidence cannot close the original macOS NFS W55 incident. No claim
of independent storage failure domains follows from four daemons on xefs1.

Final private broad gates: core POSIX 212 pass / 0 fail / 0 skip, concurrent
POSIX 64 pass / 0 fail / 0 skip. `final-live.log` ends with
`GC reclamation direct PASS`. No private fixture remains running.


The final ACK-watermark follow-up is in `ack/` and `ack-live.log` (ports
28440–43). Core POSIX again passes 212/212 and concurrent POSIX 64/64, plus
controlled CREATE reply loss/BUSY, negative replay, unique creates and cold GC.
The cache-pressure regression is in `unit/retry-ack.log`.

Three-VM normal deployment / Linux NFS:

| Gate | Result |
|---|---|
| 20 sequential 2922-byte cp operations / successful close | 20/20 exact bytes |
| Fresh gateway FUSE + Linux NFS mount, then after stress | 20/20 exact bytes / inode IDs |
| Physical inventory after copy | >=2 distinct erasure indices for every inode |
| Linux NFS core before CREATE ACK refresh | 172 pass, 39 fail, 1 skip |
| Linux NFS core after CREATE ACK refresh | 179 pass, 32 fail, 1 skip |

EIO cases decrease from 12 to 5 in one comparison; both broad NFS gates fail.
Remaining failures must not be represented as clean acceptance or attributed to
consensus without diagnosis. The export retains all_squash/anonuid=0; permission
expectations and NFS-specific fallocate/direct-I/O/link-count behavior also fail.
These initial clients use Linux NFS 4.2. Subsequent macOS NFSv4.0 data-only
fresh-mount verification passes 20/20 (see macos/); plain cp attribute copying
fails. The further [publication stress](../20261009-xorinox-w55-stress/README.md)
adds 640 operations and exact generation inventories. Gateway/NFS remounts clear client state; the three storage servers
were not crashed during this NFS run. Full server-crash evidence is private.

`*-runtime.json` capture final installed processes, efs UID, direct-I/O flags,
TCP environment, disabled commands opt-in and running executable hashes.
`installed-before-ack/` retains the earlier process captures.
`deployed-after-ack.txt` hashes the final source and normal binaries; its client
has no compiled CREATE fault hook. Storage processes were preserved during the
ACK-only client roll. The initial start log ends at a local SSH-config sandbox
write denial; permissions were granted and `cluster-restore.log` completes.
No store was wiped or unpublished client body force-discarded.

NFS cleanup logs retain both the failed sticky-directory removal and subsequent
privileged cleanup attempts; no error is silently called a gate pass.

The copied files and first failed-suite root were removed. The second owned
suite root `/mnt/efs/posix-xefsct1-v703fs9t` remains for diagnosis because its
deep chain fails readdir/removal even through the privileged gateway view.
`nfs-deep-cleanup.log` records the final unsuccessful attempt. The extra NFS
mount was successfully unmounted/removed. No broad delete or store reset was
used to hide this namespace failure.
