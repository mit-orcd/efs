# Three-package acceptance round — Oct 7, 2026

**Later follow-up:** [memory acceptance](memory-closure-20261007.md) closes
W59/W67. Counts and remaining memory gates below record the earlier pass;
current totals are 46 open packages, 10 awaiting gates.

Requested packages: W59/W67, W84 and W23/W89. Baseline `ceadd6d2` plus
existing unrelated benchmark/documentation changes; fixture manifests capture
source and executable hashes. Tests use the NUC's private
four-node TCP fixtures and owned storage directories on SATA `/data1/efs`
and NVMe `/data2/efs` (the latter resolves into `/home`). Normal service
stores and other machines are untouched. These are acceptance experiments,
not a production deployment or a general process RSS guarantee.

## Count reconciliation

| Open category | Grouped packages |
| --- | ---: |
| Unfinished correctness / investigations | 15 |
| Implemented / partial awaiting gates | 11 |
| Unresolved decisions | 3 |
| Future features | 18 |
| Total | 47 |

All three packages advanced; none is fully closed by this round. Final direct
and buffered namespace fixtures each pass 216 single-client and 64 peer POSIX
cases, zero failures and one expected skip. Both stricter publication-pressure
repeats and a clean Linux unit run also exit zero. The prior
[next-five ledger](next-five-gates-20261007.md) remains the count baseline.

## W59/W67 — retained publication ownership under pressure

The fault client reads an owned REPORT fault file; `WITHHOLD ALL` withholds
chunk publication through the existing production ownership/retry path.
With 32 MiB normal + 32 MiB drain budgets, direct and buffered runs each
accept 245 sparse writes, then refuse further admission and fail fsync while
publication is withheld. After `OFF`, fsync recovers and a cold remount
verifies every accepted range. The stricter repeat additionally checks that
the refused write did not extend the file. Physical deletion, GC queues,
quota zero and restart/late-PUT fencing are checked by the whole fixture.

Final stricter repeat peaks: direct 89,816 KiB; buffered 89,808 KiB.
Initial fault-phase peaks: direct 89,852 KiB; buffered 89,824 KiB. Traced
admission retains 32,112,640 live bytes, zero reservations, 33,554,432 backing
bytes, under the 67,108,864 combined limit. The later concurrent sparse phase
peaked at 123,140 KiB in the initial direct run. These are sampled workload
measurements, not allocation-wide RSS ceilings.

The production allocator unit fills both the normal and drain capacities,
refuses a further allocation, retains failed-body charges, and releases them
only on free. This is real reserve exhaustion in the allocator test, separate
from the live failed-publication gate; do not call it live recovery-reserve
exhaustion. A small-memory-host/cgroup gate and combined live reserve failure
remain owed. No fault hook is enabled in a normal build.

## W84 — correct refusal and finite progress

The ancestry walker now returns BUSY when it exhausts capacity without
reaching the root; it reports INVAL only for an observed cycle. A non-root
self-parent is rejected instead of being treated as a root. Production-helper
coverage exercises 64 guards on one shard, the 65th-hop refusal, exact
participant capacity, unchanged canaries and failed ReadIndex authority.

A single monotonic eight-second budget now covers all nested rename RPC
retries, preserving any tighter caller deadline. The previous live attempt
failed the 30-second refusal bound; nested budgets could spend approximately
80 seconds on a permanent refusal. Final direct and buffered fixtures accept depth 63,
refuse depth 64 in 8.003 seconds, preserve the source and destination,
restart all four members, and move the retained directory back successfully.
Depth is a measurement of this fixture's participant placement, not a promised
product depth. Opposite-direction cycle races and empty 16-lane operations
are included, as is the safe pre-PREPARE rejection of a full 64-lane directory.

The fully spread directory still cannot fit its guards plus fixed participants
in the existing envelope. Supporting that work requires a larger/coordinated
predicate envelope or a different protocol; returning BUSY is not completion.
Concurrent leader failure at the maximum envelope is also not established by
a full restart. The package remains open.

## W23/W89 — actual production cap and recovery defects

A separate production KV-engine experiment reaches the unchanged 1 GiB L0
cap: 128 segments, 1,073,744,000 L0 bytes. It uses synchronous WAL persistence
and a compile-only compactor park on the NUC; this is not a Raft follower-lag
experiment. A finite 64 MiB tail grows the memtable to 67,109,312 bytes and
RSS to about 92,950 KiB. Thus the cap does not bound the memtable while
compaction stays parked. W23's general follower memory/lag acceptance remains
unproven; do not interpret the finite tail as a growth limit.

Two real defects are repaired:

- Once compaction frees capacity, its worker flushes an oversized deferred
  memtable even if no additional application write arrives. Before the change,
  the fixture timed out after 120 seconds with the retained memtable. The
  repaired experiment drains in three seconds and verifies bytes on reopen.
- A flush no longer truncates WAL records durably appended by another thread
  before that thread obtains the memtable lock. Reset checks the applied
  sequence under the WAL mutex; an unapplied tail retains the complete WAL.
  The applied watermark advances before the current batch's flush while the
  engine mutex still prevents another batch from applying. A deterministic
  actual-crash test reproduces lost overwrite, insertion and deletion with
  the old binary and passes all three with the repair.

Four additional 64 MiB update rounds keep the post-recovery sampled RSS near
200,940 KiB, with empty memtables and normal L0 counts. Retained allocator
memory explains why drain is not a promise to restore baseline RSS. A final
engine repeat includes the WAL repair. W89's strict sample-schema tests and
all Linux units pass; the original malformed TSV evidence remains retained.

## Test infrastructure findings

Repeated buffered remounts exposed two natural-retirement races. Discovery
now rereads process state/start identity if cmdline becomes empty during exit.
Retirement waits up to ten seconds for natural exit when the control socket
has already closed; it never signals without endpoint ownership. PID reuse,
ambiguous live commands and unowned live endpoints still fail closed.
Deterministic fixtures cover these cases, including a socket-closed client
that exits naturally. Relative fault-binary arguments are normalized to
absolute paths before spawning.

Raw commands, logs, fixture manifests and metrics are in the
[evidence directory](../../results/measure/20261007-three-package-gates/SUMMARY.md).
Retained failures include the original depth assumption, slow refusal,
retirement races, old KV timeout/data loss, and setup mistakes (missing copied
document evidence, copied macOS test binaries and a relative binary path).
They are not counted as passing gates. Final outcomes are recorded there.
