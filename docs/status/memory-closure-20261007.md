# W59/W67 acceptance closure — Oct 7, 2026

**Closed:** the grouped W59/W67 cache-admission and retained-body ownership
package. Test-only closure/gate commit `4e340b7b`; the allocator/publication
repairs were already implemented. Private NUC four-node TCP fixtures only;
no production deployment, default-budget increase or discard policy change.

## Final live gate

| Mode / body+drain MiB | Accepted sparse writes | Fault client RSS MiB | Whole-fixture peak MiB | OOM events |
| --- | ---: | ---: | ---: | ---: |
| direct / 32+32 | 245 | 120.3 | 242.1 | 0 |
| buffered / 32+32 | 245 | 120.4 | 251.9 | 0 |
| direct / 256+64 | 2037 | 378.4 | 494.1 | 0 |
| buffered / 256+64 | 2037 | 378.4 | 906.3 | 0 |

Both configurations run under a real, inherited 1 GiB cgroup ceiling with
zero swap allowance. The default-value variant explicitly configures the
production 256+64 MiB values. Whole-fixture peaks include four servers,
clients and kernel file cache; they are not client RSS or an allocator-only
ceiling. Exact samples are in the [raw evidence](../../results/measure/20261007-memory-closure/SUMMARY.md).

An owned compile-only fault retains real scratch allocated by the production
pool and touches its pages. Live charges reach exactly 64 or 320 MiB, including
the application's still-owned bodies, with zero reservations. The next drain
allocation refuses ENOMEM while publication is withheld. Failed fsync does
not claim publication; the refused write does not extend the file. After
release, fsync and new write admission recover on the SAME mount. Cold remount
verifies every accepted range and the new file. Eight concurrent sparse
writers, physical GC effects, queues, restart/quota and late-PUT fences pass.
Normal/fault allocator units pass, and a normal compiled module contains no
scratch-exhaustion hook.

These close the previously outstanding combined live reserve-failure and
small-memory acceptance, together with the prior sustained-write, metadata
reclaim, append/concurrent and full normal POSIX/unit evidence. The
[three-package ledger](three-package-gates-20261007.md) records the earlier
216 single-client / 64 peer POSIX cases in both I/O modes. No data are discarded
by the fault/recovery gate. D27 whole-call timing, broader recovery/session
fencing and unrelated cache growth remain independent open work.

## Updated grouped count

| Open category | Previous | Now |
| --- | ---: | ---: |
| Unfinished correctness / investigations | 15 | 15 |
| Implemented / partial awaiting gates | 11 | 10 |
| Unresolved decisions | 3 | 3 |
| Future features | 18 | 18 |
| Total | 47 | 46 |

## Next work

**W84 first:** support all 64 directory-lane emptiness guards plus fixed
participants and all required ancestry predicates. The current envelope
still permanently refuses valid full-lane rmdir/replacement; it is an
implementation gap, not merely an unrun gate. Preserve every guard, coordinate
wire/storage bounds and recovery, then test concurrent creates and coordinator
leader failure around PREPARE/decision. A bigger retry budget does not fix it.
The 64-hop ceiling also needs a supported full-ancestry protocol or an approved
product limit; do not silently discard deeper ancestors.

**W23/D30 next:** the memtable can still grow while compaction remains stalled.
Recommend bounded admission before new Raft proposals plus replica pacing,
with finite queues and follower catch-up tests. Already-committed entries must
remain durable and cannot be rejected during apply. The decision register
still calls D30 an ask and states “Forbidden. code before the decision”;
record the chosen remedy before implementing that policy. See
[D30](decisions.md#p25--d30--the-apply-lag-remedy-performance-plan-row-ask).
