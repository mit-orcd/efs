# D25 bounded receipt retirement — 2026-10-06

Implemented staged PUBLICATION_RETIRE (117/118), Raft command 29, and a compact
FileID/chunk/mount-session replay floor (KV kind 27). An exact acknowledgement
verifies the canonical digest and retires the lowest stored sequence. Receipt
DEL and floor PUT are atomic. Lost ACK replies are idempotent; old publications
return RETIRED and never reapply. A new stream request at 64 outstanding receipts
returns BUSY, while stored retries remain recoverable. No timer evicts evidence.

RETIRED is distinct from COMMITTED/REJECTED and is not an original-outcome proof.
The transport accepts it; the dirty-cache ownership boundary retains token,
body and pin. A retire operation cannot answer success merely because its old
COMMITTED receipt is still present after an ambiguous proposal.

## Validation

Isolated Linux source/build directory on `nuc_efs`:
`/tmp/efs-d25-retirement-20261006`; no deployed sources, mounts or daemons changed.

- Production efsd and efs-fuse builds PASS.
- Publication recovery PASS: terminal outcome/digest identity, 64-receipt cap,
  capacity recovery after ACK, out-of-order ACK BUSY, atomic storage-failure
  retention, repeated ACK, below-floor unpublished identity rejection, and
  LSM replay after child process exits without closing following retirement.
- Metadata apply and session regressions PASS.
- Production host adapter and client RPC boundary regressions PASS, including
  retired status recovery, missing retirement outcome and no replay proposal.
- Client memory/read-pressure regressions PASS, including RETIRED retaining
  accepted body, pending identity and pin.

The normal Mac Makefile is incompatible with its existing Linux fopencookie
usage. Host/RPC extracted production tests also passed on Mac; Linux is the
production build validation. No live-cluster POSIX or throughput gate was run. A Linux ASan/UBSan build
was attempted but could not link because the NUC lacks libasan.so.8.0.0;
sanitizer execution is not claimed.

## Remaining activation work

The cap bounds live receipts per stream, not total streams or physical LSM
scan/tombstone work. Compact floors must remain until old sessions are fenced.
No abandoned-session cleanup is enabled: existing I23 session admission/fencing
must cover staged publication endpoints before deleting old-epoch evidence.
The client also needs a bounded, ordered exact-ACK retry queue after consuming
terminal results; never retire past an older locally pending intent.

Then connect authoritative admission and both production flush paths, finish
lane mtime invalidation, and run live shrink/extend, leader-change, lost-reply,
client-restart and sweep/history-retirement gates. D25 remains staged.
