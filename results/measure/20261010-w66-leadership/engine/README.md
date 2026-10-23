# W66 quorum-preserving leadership transfer — Oct 10, 2026

AMD direct/TCP private source; build identity is `unknown` (no `.git` remotely).
No installed topology or protection change. Raft/wire tests pass; fatal
ASan/UBSan with leak checking passes. State machine, memory store and tests
are instrumented; other archive dependencies are not.

The source fences itself before sending TimeoutNow. A stable, recently
responsive and caught-up voter may start an ordinary election; no trigger
confers authority. Tail, configuration, known leader and exact term are
checked. Three-/four-voter quorum rules, durable term/vote and campaign/pressure
fences still apply. Lost/failed sends never restore source authority.

Tests cover lagging targets, stale idle replication matches, rejoined peers,
joint configurations, delayed/wrong-tail/wrong-membership/nonleader triggers,
campaign prohibition, pressure refusal and transport failure. A two-of-four
partition cannot elect or acquire read authority; normal quorum recovery
preserves the committed bytes. Wire roundtrip preserves transfer fields.

Commands on `/data1/efs/evidence/merged-storage-src-20261010`:

```
make -j8 efsd efs-mgmt tests/test_raft tests/test_wire
./tests/test_raft
./tests/test_wire
gcc -O1 -g -std=c99 -D_GNU_SOURCE -fno-omit-frame-pointer -fsanitize=address,undefined -Wall -Wextra -Iinclude -Isrc/common -Ideps/blake3 -I. tests/test_raft.c src/raft/raft.c src/raft/raft_mem.c libefs.a -lpthread -lm -ldl -libverbs -o /tmp/efs-w66-balance-asan
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 /tmp/efs-w66-balance-asan
```

Tested SHA256:

```
e1f097a36fc9b3ba64bf79cdf97481100be0dfb54928931561a261ff5d17bdc7  include/efs/raft.h
9f3c7873267f5230906ff601137575167a75650a48cd749efe8d908303f59a7a  src/raft/raft.c
7a522960e9eba35f8bb8198d7d05267ea0a438f5695707869de112f7f38505cf  tests/test_raft.c
d2c88f243d3f74b9bdc2074fb97b70dd71e062aea3321d997b66998e30f1b6cc  tests/test_wire.c
```

This implements leadership handoff, not D36 commit certificates, recovery
without a quorum, versioned shard routing or additional independent groups.

**Raw evidence note (2026-10-23):** the bulk data in `merged-storage-src-20261010` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
