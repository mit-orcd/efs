# Publication and applied-index persistence boundaries

Production repairs:

- Legacy/typed publication/admission/retirement and activation IO/NOMEM retain
  the pending Raft apply index. Business conflicts remain ring verdicts.
- Legacy publication batches honor hold/release errors and never hide a later
  storage failure behind an earlier STALE result.
- Periodic and forced shutdown applied-index checkpoints require successful
  KV WAL release. A latched WAL failure leaves the durable Raft tail replayable.

AMD direct/TCP normal fixture gc-direct-2p7_ktz4: core212/212, peer64/64,
exact800 cold append records, retained REPORT/lease/ticket/namespace gates and
physical reclamation/restart pass. Complete code regressions exit0; Linux build
has no warnings. Extracted actual apply policy, batch verdict and checkpoint
helpers pass failure/retry/forced-shutdown tests.

Actual LD_PRELOAD fault fixture gc-direct-fv1rzcf7 refuses follower3's KV WAL
fdatasync, captures the previous applied records at the refusal boundary,
checks neither checkpoint changes through later accepted writes and clean
shutdown, then removes the fault and cold-restarts. The follower catches up;
exact bytes, eight follow-up files, inode identity/nlink2 and physical deletion
queues pass. See JSON, manifest and live log. The initial hook intercepted
fsync rather than the WAL's fdatasync and timed out without injecting a fault;
that fixture restored service, then the hook was corrected and rerun.

This is actual syscall refusal/restart evidence, not hardware power-loss proof.
Installed service restored and reservation released after both fixtures.
