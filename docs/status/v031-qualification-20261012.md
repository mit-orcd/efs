# v0.3.1-pre-alpha integration and qualification

User requested merging all known branch commits into `release-v0.3.1-pre-alpha` and repeating ordinary-files/TCP/direct qualification. Root works alone. No push, tag, main/devel promotion or repair activation.

All captured branch tips, including the93 previously unmerged unique local commits, are ancestors of this branch. [Exact branch snapshot](v031-branch-integration-20261012.json). Remote-tracking refs were already contained. Uncommitted work is not imported. Historical evidence remains source-specific; merging it does not transfer passes to this candidate.

Conflict resolutions retain the newer staged ReadIndex completion implementation, the qualified32-frame bounded FIFO prefix, and the latest user scope. The older superseded read-proof and receive-limit variants do not replace them. NUC's historical receiver is reconciled with the newer default-off ORCD manual coordinator. Opt-in phase profiling measures each bounded parallel fragment PUT without reverting parallel uploads. Experimental query/repair code is present but remains outside live qualification scope.

## Required frozen-source checks

- Full normal build/native suite, early-ACK Raft, fault build/native suite, affected ASan/UBSan checks.
- Four AMD daemons with explicit4G quotas each, files/TCP/direct, seven clients/jobs16, original216-case core suite and30-second per-case budget; peer and clean shutdown checks.
- Ordinary span-to-full overwrite, primary/peer/fsync/cold exact bytes and physical fragment/metadata queue reclamation; not dedicated repair/recovery qualification.

Every live process uses client `EFS_COMMANDS=0`, daemon `EFS_COMMANDS_SERVER=0`, and `EFS_REPAIR_EXPERIMENT=0`. .efs, RDMA, buffered and dedicated repair/recovery remain excluded. No profilers/builds overlap quiet live tests. Private fresh fixtures leave unrelated AMD services untouched.

## Final frozen-source verdict: PASS

Exact tested source: **15e2a5310df1dd0fd4e307a9508c95d513d14672**. Integration and qualification complete for ordinary files/TCP/direct only. Later documentation/evidence commits are not a new product/test identity.

| Gate | AMD result |
| --- | --- |
| Normal build/full native | PASS; native92.9s |
| Fault build/full native | PASS; native92.3s |
| Early-ACK Raft | PASS |
| Eight affected ASan/UBSan extractors | PASS |
| Fully instrumented real Raft build/native/early-ACK | PASS |
| Seven clients, core216 each, jobs16 | **1512/1512 PASS**, no skips/notrun |
| Deep-directory original30s deadline | **7/7 PASS** |
| Two-client peer cases | **67/67 PASS** |
| Graceful client stops | **7/7 PASS** |
| Ordinary overwrite primary/peer/fsync/cold exact bytes | PASS |
| Physical fragments and metadata queues drained | PASS |

The full quiet live fixture took63.733s including setup, suites, peer and shutdown; individual case deadlines were unchanged. Isolated overwrite/cold/reclamation took17.361s. This is controlled cold restart byte validation, not dedicated repair/recovery qualification. Its external wrapper records `acceptance:false` for broader acceptance, while the focused checks pass.

All **28,065 regular tracked source inputs** matched before and after testing. Normal/fault binary hashes retained. **101 raw evidence/controller files** verified against independently computed remote SHA256 hashes. Runtime configuration records verify commands0/server0 and experimentalrepair0. Servers explicitly select files and direct I/O. Root worked alone; no profilers/builds overlapped quiet measurements. All private processes/mounts stopped and unrelated AMD services left untouched.

[Raw evidence](../../results/measure/20261012-v031-15e2a531/README.md). No push, tag or main/devel promotion. No passing result is claimed for excluded .efs, RDMA, buffered or dedicated repair/recovery. Historical failed/excluded experiments remain preserved and source-labelled.

The all-branch snapshot excludes uncommitted work and the existing generated-document stash (not a branch tip). No stash was applied or dropped.

## Additional user-requested TCP/buffered rerun

[Buffered report](v031-buffered-qualification-20261012.md): same frozen **15e2a5310df1dd0fd4e307a9508c95d513d14672**, ordinary files with .efs/experimentalrepair disabled, full normal/fault native and sanitizers PASS, core1512/1512, deep7/7, peer67/67, seven stops and ordinary cold/reclamation PASS. Explicit4G fixtures and original deadlines unchanged. Initial disk-headroom startup refusals retained; completed private build/store cleanup restored headroom before fresh passing retries. No code changes, pushes or tags. This later request adds mode-specific buffered validation to the original direct-I/O qualification; it does not qualify other excluded features.
