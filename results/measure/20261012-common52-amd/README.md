# Common52 AMD qualification

Exact commit52bcfd3570b0a59076c225caad9157b0825954d8 on isolated
qualification-v030-tcp-direct. All23,318 tracked regular inputs hash-verified,
zero mismatches. Full normal and fault native suitesPASS0 (~90seconds each).
Actual Raft normal/earlyACK and fully ASan/UBSan-linked Raft normal/earlyACKPASS0.
These are native prerequisite passes, not mounted seven-client acceptance.

Supplemental actual-core whitebox context fixture initiallyFAIL because a direct
snapshot-finalizer test passed a zero/invalid response envelope to real transport.
Corrected reply envelope with isolated delivery thenPASS, unchanged productsource.
Tests populate both proof records and exercise real membership A→B→A transitions,
actual snapshot persistence EIO, and direct lifecycle/import callbacks. The latter
callbacks are whitebox checks, not distributed snapshot/election acceptance.
Supplemental context sanitizerPASS0, marker and full command retained.

The read waiter still rechecks its coherent view while holding cv_mu before sleep;
wakeup takes the same lock. Review finds this locking unchanged, not a new
full-host CV integration regression. Packing/query correctness remains blocked
by Intel seven-client failures; this candidate is not release accepted/integrated.

Native execution counts per mode:247 Python recipe lines and70 native binary invocation lines. These are executions, not distinct suites.
