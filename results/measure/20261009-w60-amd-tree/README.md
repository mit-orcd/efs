# W60 AMD mixed-tree acceptance — Oct 9, 2026

Private four-daemon AMD direct-I/O/TCP fixture: `gc-direct-8k8lelm8`,
ports 28840–28843. Installed stores are preserved; only the two approved mounts
are borrowed and drained. No NUC workload is touched.

The expanded read-pressure gate cold-remounts a nested tree containing 2000
three-byte files and four 128 MiB files. It walks and verifies every byte while
another reader continuously verifies 128 KiB sequential reads. Client logs
confirm body hard limit, drain reserve and slab limit each 33554432 bytes.
Those are separate budget classes, not a 32 MiB whole-process RSS claim.

`mixed-read.json`: 2000 tiny files, 536870912 large-file bytes and 1084096512
concurrent sequential bytes verified in 4.211 seconds. No tiny/large failure,
no sequential error, reader stopped. This is correctness evidence, not an
isolated performance benchmark. Existing prefetch admission code is unchanged.

Candidate binary SHA256:
- efs-fuse: `a2557770ae79779ffccb9b72b9deb4c1fd724ac95667013148e3eb45b9046bd1`
- efsd: `587c84bf41cb0a260bf310516e7b98928f58eab4dd0c32acc4b414a156689f37`

The historical Xorinox deployment result is not claimed. Demand/dirty-owner
exhaustion remains outside this speculative-starvation acceptance.

Full follow-on acceptance passes: core POSIX212/212, peer64/64; cold fold/live
base reads; offline-member reap-ledger retention and rejoin; final cold restart,
zero physical inventory/quota and late PUT fencing. POSIX deletion counters
advance from 24 to 166 GC passes and 270 to 22374 removed fragments. See
`posix-gc-effect.json`; both before/after physical data usage is zero after drain.
This measures the POSIX phase, separately from the earlier mixed-tree deletion.

Installed AMD978caaf7 direct/TCP service remained intact. Both approved mounts
were independently verified restored; reservation released. On Oct 9 the user accepted AMD as the functional gate environment: W60 is
closed on this evidence. Xorinox deployment verification is optional.
