# W94 guarded session wire gates — AMD, Oct 8

One host/four daemons; direct-I/O/TCP, unique disposable private stores.
Installed stores retained and installed service restored after each completed run.
No production FUSE piggyback activation, hardware power-loss or distributed
hardware-RDMA claim.

- `gc-direct-gfy2f_ho`: first guarded wire/full regression PASS.
- `gc-direct-mma_iq6m`: unknown probe/no ownership and global-fence reclaim added;
  guarded/full regression PASS.
- `gc-direct-ejr5ca22`: explicit NOT_PRIMARY/no hint on ESTABLISH after cold rejoin;
  failed closed, installed service restored. Failure log retained.
- `gc-direct-1vk60ch4`: bounded explicit no-hint admission retry plus local-fence
  replay-window retirement; guarded/full regression PASS.
- `gc-direct-mgwf9eza`: private CREATE apply IO/NOMEM injection FAILED after
  cold restart: `guarded-lease` CREATE returned EBUSY. Leader retried index 32,
  but followers applied only the first injected IO; unchanged committed
  heartbeats did not retry local apply. Original leader/follower logs retained.
  The live wrapper log was overwritten by the repeat; no reconstructed log is
  presented as an original. Installed service was restored.
- `gc-direct-5dywwrr_`: corrected fault repeat PASS. IO/NOMEM markers are
  present on the follower; the same committed index is retried to completion.
  Guarded ownership, POSIX/peer, second cold WAL recovery, physical inventory and
  quota zero all passed; installed service restored.

Every successful full run includes POSIX 216/217 (0 fail, 1 skip), peer 64/64,
rejected FUSE replies, fsynced tiny churn, local/first-use hashed CREATE leases,
append replay/sync, dirty-stop recovery, graceful leader shutdown, physical GC,
invalid WAL tails plus second cold acknowledged-byte verification, quota zero
and late PUT fencing. These are live gates, not a claim of complete W94 client
adoption/session activation.

JSON includes original sequences, ACKs, expected verdicts and read-only queries.
The deliberately discarded OPEN reply is resolved through PROBE before any
resubmission of that sequence. The two session-owner groups differ across runs,
including a cross-group owner/inode case. Cold restarts replace every private
server; they do not simulate machine power loss.

Complete Linux test target logs are retained separately. Early unit-tree
attempts lacked documentation/evidence dependencies; those were supplied from
committed archives and the complete target passed. No test was weakened.

`private-create-apply-fault.patch` is a test-only patch applied exclusively to
`/data1/efs/evidence/w94-create-apply-fault-20261008`, built from the committed source plus
the CREATE apply storage-error fix. It returns IO then NOMEM immediately before
the first atomic CREATE batch per private process. Production sources/binaries
have no fault hook. Actual phase markers must be present in daemon logs.

Latest session/metadata ASan/UBSan also covers all 120 pending-adoption/normal
OPEN/CLOSE/cleanup arrival permutations, exact read-only stored verdicts,
contiguous retirement and final inode reclamation. Normal KV library linked;
not a fully instrumented library or a production FUSE adoption gate.

**Raw evidence note (2026-10-23):** the bulk data in `w94-create-apply-fault-20261008` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
