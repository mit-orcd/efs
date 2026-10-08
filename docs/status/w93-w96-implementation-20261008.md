# W93–W96 implementation checkpoint — Oct 8, 2026

Baseline: `devel` merge `d4e70492` includes `e6926d85`. Work continues in
logical portions; none of the four performance packages is closed by this
initial checkpoint. Functional gates use AMD direct-I/O/TCP, one host/four
processes. Installed stores and binaries are preserved and service restored
between private fixtures. No physical power-loss or distributed-RDMA proof.

## W96 recovery prerequisite

Fixed replay leaving discarded bytes in front of later acknowledged writes.
Recovery validates a complete batch before callbacks, removes a torn/CRC-invalid
trailing record and durably establishes the valid EOF before append opens.
I/O and CRC-valid structural errors refuse recovery without truncating evidence.
A partial append or sync failure fences the WAL instance against later success
until reopen; reset/hold cannot erase or bypass that failure.

Validation: original production-WAL reproducer now recovers A+B. Repeated
process-crash tests cover short headers/payloads, zero padding and invalid CRCs;
post-repair acknowledged writes survive two further opens. Fault tests cover
ENOSPC after a partial write, sync EIO and malformed atomic batches; normal and
ASan/UBSan pass. AMD KV-LSM and Raft-store suites pass.

Private AMD `/data1/efs/gc-direct-20dpw3sk` passes full POSIX 216/0/1, peer 64/64,
append/sync/retry/stop/restart/GC gates, and deliberate invalid tails on all four
WALs followed by new fsynced files and a second crash. Both files match exact
cold bytes. Physical inventory/quota return to zero and late PUTs stay fenced.
The live fixture contains the tail-repair/validation fix; final sticky I/O
fencing was separately exercised by the focused AMD/Linux and sanitizer tests.

[Retained acceptance evidence](../../results/measure/20261008-w93-w96-implementation/w96-recovery/live.log).
KV preallocation and its performance/recovery gates remain separate work.

## Remaining packages

- W93: bounded cross-request follower batching, with ACK/apply/read visibility
  gated by persistence; measurement and faults before acceptance.
- W94: atomic CREATE/open lease, safe adoption and all namespace paths/retries.
- W95: extend existing independent storage experiments for true tiny logical
  payloads, durable completion latency and mixed churn; keep production separate.
- W96: bounded KV preallocation, recovery/reset/compaction/fault gates and matched
  performance measurements after the recovery prerequisite.

## W96 bounded preallocation

Linux durable WALs reserve EOF in bounded 1 MiB increments and write at the
logical valid EOF with pwrite. NOSYNC and other platforms retain exact growth.
Allocation failures disable the optimization and fall back to ordinary writes;
partial writes or failed barriers still fence the instance. Replay removes
reserved padding; reset clears physical and logical lengths together.

Focused AMD KV/Raft-store and fault tests pass, including unsupported allocation,
ENOSPC fallback, multiple reservation boundaries, reset/reopen, partial-write and
sync faults. Local ASan/UBSan passes. Private AMD `gc-direct-i0zfg2cp` passes
POSIX 216/0/1, peer 64/64, zero-tail repair and a second crash after new fsynced
files, plus final empty physical inventory. Installed service restored.

Three matched 1-KiB durable-append pairs: QD1 baseline 4016–4163 ops/s vs
preallocated 6182–6941; QD8 baseline 16727–17161 vs 17816–18046. This supports
keeping the bounded optimization on AMD, not claiming a NUC/two-device result.
[Measurements and fixture evidence](../../results/measure/20261008-w93-w96-implementation/w96-preallocation/README.md).
