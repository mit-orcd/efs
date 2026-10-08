# W66 outstanding-batch ReadIndex regression

Baseline production code: 532660e3 (same runtime code as 3f8c9f81).
Run locally on the Mac, using an isolated in-memory three-voter Raft network.
This is deterministic protocol evidence, not AMD latency or Linux acceptance.

The new test delivers a durable leader's data batch to followers but withholds
its replies. It then releases reply delivery and begins a fresh read round,
without advancing timer ticks. Baseline suppresses outgoing messages because
commit has not advanced and the prior batch is fresh: the new read stays pending.
The fix sends a context-bearing empty probe through existing commit-probe logic;
the fresh quorum completes immediately without resending any data batch.
Existing tests retain rejection of old-round ACKs, partitioned leader authority,
higher-term fencing, bounded catch-up and durable follower batch ACK ordering.

Commands (from repository root):

```
clang -O2 -g -Wall -Wextra -Werror -Iinclude tests/test_raft.c src/raft/raft.c src/raft/raft_mem.c src/wire/wire.c -o /tmp/w66-raft
/tmp/w66-raft
clang -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Iinclude tests/test_raft.c src/raft/raft.c src/raft/raft_mem.c src/wire/wire.c -o /tmp/w66-raft-sanitize
/tmp/w66-raft-sanitize
```

Baseline compilation substitutes `git show 532660e3:src/raft/raft.c` while keeping
the new test. Expected baseline exit 1; fixed and sanitizer exits 0. No measured
wall-clock improvement or causal attribution of the NUC group split is claimed.
