# Task CT: test_barrier_backoff

[Task CT: test_barrier_backoff](../store-durability-backoff-window-20261103.md): **test defect fixed, no K row, blocks alpha: no** (Nov 3, node9900): the fixed 1.2 s sleep is replaced by a bounded wait on `failures` with exact back-off schedule checks, plus a leaked-syncer-thread fix in the test harness; old test failed 2 of 10 under gate load, new passed 10 of 10 and 60 of 60 solo; the gate showed only the known K54 port-race flake (Task CS).
