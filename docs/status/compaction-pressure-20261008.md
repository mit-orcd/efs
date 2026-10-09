# D30/W23/W89 — Mac pressure foundations, Oct 8, 2026

Later integration: [AMD admission, real-cap follower and cold FUSE gates](compaction-pressure-amd-20261008.md).
This file preserves the earlier Mac checkpoint; its unimplemented integration
items describe that checkpoint, not the later branch implementation.

Branch `devel-compaction-pressure`, base `532660e3`. Package remains open.
This checkpoint implements engine observation and a reservation primitive;
production Raft admission and follower pacing are not activated.
Foundation implementation: `15010827`.

## Implemented

`efs_kv_lsm_pressure` captures the memtable, L0 shape, configured flush target
and poisoned persistence state under one KV lock. L0 soft pressure begins at
768 MiB; the unchanged hard L0 cap is 1 GiB. An oversized memtable (twice its
flush target) also reports hard engine pressure. These are engine observations,
not process RSS bounds. Committed apply remains unconditional with respect to
these signals; no new rejection was added to WAL replay or apply.

`efs_admission` is a nonblocking, mutex-serialized count/byte reservation
primitive. A trusted recovery class can use a reserved portion of the total
budget. Ordinary admission cannot spend that reserve. Arithmetic rejects huge
charges without overflow, exhaustion returns BUSY without a ticket, and
persistence failure returns IO. Accepted tickets remain charged when pressure
or failure changes. They are returned only by their owner after apply or a
definite abort; cancellation of a waiting RPC is not a definite abort.
There is no internal waiting queue. Caller retries must inherit the original
deadline. A ticket is noncopyable by contract and cannot be concurrently released.

The primitive has no production call site or production limits yet. Unit-test
budgets (100 bytes, 10 operations, 20 bytes/2 operations reserved) are deliberately
small boundary fixtures, not proposed deployment limits. The 24-thread test
makes 480,000 attempts and checks total bounds under contention.

Engine hard L0 pressure must **not** be blindly mapped to global hard admission:
ordinary producers should stop early while independently charged recovery and
cleanup can spend their reserve. A global hard memory/log budget or poisoned
persistence prevents even new recovery admission. Reserving cleanup operations
requires conservative KV expansion charges, not command wire lengths alone.

W89 adds `w23_samples.py cap-proof`: at least two complete three-replica ticks,
known RSS/memtable/L0/lag and roles, no duplicate observations, and a follower at
the unchanged 1 GiB L0 cap. Missing data stays unavailable. This structural gate
proves that pressure was observed; it cannot prove memory, latency or recovery
bounds. Historical malformed evidence is preserved.

## Mac evidence

The private fault-build experiment ran as Linux `efs` on Tart `efs1`, in
`/data1/efs-pressure/w23-20261008.nMOJ9X`, separate from installed stores. It
uses the production synchronous KV WAL and compile-only compactor park.

- At 805,308,000 L0 bytes: soft=1, hard=0.
- At 1,073,744,000 L0 bytes / 128 segments: soft=1, hard=1; RSS 36,248 KiB.
- Eight additional committed 8 MiB values: memtable 67,109,312 bytes and sampled
  RSS 85,272 KiB. This finite tail is growth evidence, not a bound.
- Fault release without another write: idle drain in two seconds, memtable zero,
  pressure clear. Recovered sampled RSS is 258,652 KiB; freeing live state does
  not promise allocator RSS returns to baseline.
- Four update/recovery rounds and exact durable reopen bytes pass.

[Raw engine log](../../results/measure/20261008-mac-compaction-pressure/kv-pressure.log).
This is an engine experiment, not a stalled Raft follower or hardware power-loss
experiment. The private rsynced build reports version `unknown`; identify source
by this branch/commit and the retained source checksums, not that version string.

## Validation

Full Linux `make test` passes on Mac `efs1`; the isolated fault-build KV suite
and admission test pass. Local admission tests also pass with AddressSanitizer
and UndefinedBehaviorSanitizer. The five W89 Python tests pass, including refusal
of incomplete cap evidence. An initial new-test failure left a durable fixture
key for the next scan-order test; fixture cleanup was repaired before the passing
full gate. [Full test log](../../results/measure/20261008-mac-compaction-pressure/mac-pressure-final-tests.log)
and [private fault-build log](../../results/measure/20261008-mac-compaction-pressure/mac-pressure-private-build.log).

The restored Mac storage cluster runs all three daemons as `efs:efs`, with
matching binary SHA256 `dc8cb6970c6920d1810e536a8a7bf2dd4539834c82fd59074b14c49405f56120`,
direct I/O/TCP and explicit `EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`.
Both groups converge at commit=applied on all nodes after the rolling restart.
Gateway `efsgw` remains stopped; no Mac NFS or FUSE acceptance is claimed.
[Restored health](../../results/measure/20261008-mac-compaction-pressure/mac-pressure-health.log),
[policy](../../results/measure/20261008-mac-compaction-pressure/mac-pressure-policy.log),
[source checksums](../../results/measure/20261008-mac-compaction-pressure/source-sha256.txt).

## Required integration contract and remaining gates

The [package ownership rules](agent-packages-20261008.md#devel-compaction-pressure)
require agreement with the W66 owner before modifying raft_host or Raft core.
D30 also requires D29/W88 durable receipt foundations before activation. No
shared pump or Raft code was changed at this checkpoint.

1. Agree the admission owner/interface and conservative retained-memory charges.
   Bound outstanding proposals, waiting callers, accepted-but-unapplied entries,
   client queues and forwarding queues. Distinguish local commit/apply lag from
   per-peer replication lag and physical WAL/log bytes.
2. Bind tickets to durable proposal identity, not a stack-scoped RPC lifetime.
   Leader change, lost replies and cancellation cannot release possibly committed
   work. Release on definite failed append, applied terminal result or proven
   retirement. Preserve D29 receipts and durable ACK ordering.
3. Charge actual replication/catch-up buffers, stop accepting slow-follower
   buffers at explicit limits, and use snapshot recovery when log-window recovery
   is impossible. One stalled replica must not indefinitely stop a healthy quorum.
   Do not weaken membership fencing or all-member GC acknowledgements.
4. Validate numeric budgets and early watermarks against real stalled-follower
   production-cap pressure. Capture RSS, memtables, L0, WAL/log bytes, queue bytes,
   admission latency, commit/apply and peer replication lag. Compare schema-valid
   samples against raw logs. Require sustained healthy-quorum and cleanup progress.
5. Release the fault and verify convergence/snapshot recovery, leadership change,
   bounded refusal, cancellation and failed persistence. Cold-restart all accepted
   writes and compare exact acknowledged bytes. Process-crash evidence remains
   distinct from hardware durability.

The old W23 shell driver has fixed `/tmp/w23` paths, process-name SIGKILL cleanup,
admin-user launch and first-pressure stop conditions. Do not use it as the new
acceptance harness unchanged. A replacement must use unique efs-owned stores,
exact process ownership, clean drains and a sustained pressure interval. Lowering
the production L0 cap or treating NA as zero cannot close W23/W89.
