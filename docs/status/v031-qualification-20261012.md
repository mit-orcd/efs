# v0.3.1-pre-alpha integration and qualification

User requested merging all known branch commits into `release-v0.3.1-pre-alpha` and repeating ordinary-files/TCP/direct qualification. Root works alone. No push, tag, main/devel promotion or repair activation.

All captured branch tips, including the93 previously unmerged unique local commits, are ancestors of this branch. [Exact branch snapshot](v031-branch-integration-20261012.json). Remote-tracking refs were already contained. Uncommitted work is not imported. Historical evidence remains source-specific; merging it does not transfer passes to this candidate.

Conflict resolutions retain the newer staged ReadIndex completion implementation, the qualified32-frame bounded FIFO prefix, and the latest user packing exclusion. The older superseded read-proof and receive-limit variants do not replace them. NUC's historical receiver is reconciled with the newer default-off ORCD manual coordinator. Opt-in phase profiling measures each bounded parallel fragment PUT without reverting parallel uploads. Experimental query/packing/repair code is present but remains outside live qualification scope.

## Required frozen-source checks

- Full normal build/native suite, early-ACK Raft, fault build/native suite, affected ASan/UBSan checks.
- Four AMD daemons with explicit4G quotas each, files/TCP/direct, seven clients/jobs16, original216-case core suite and30-second per-case budget; peer and clean shutdown checks.
- Ordinary span-to-full overwrite, primary/peer/fsync/cold exact bytes and physical fragment/metadata queue reclamation; not dedicated repair/recovery qualification.

Every live process uses `EFS_PACKED_WRITES=0`, client `EFS_COMMANDS=0`, daemon `EFS_COMMANDS_SERVER=0`, and `EFS_REPAIR_EXPERIMENT=0`. Containers, packing, .efs, RDMA, buffered and dedicated repair/recovery remain excluded. No profilers/builds overlap quiet live tests. Private fresh fixtures leave unrelated AMD services untouched.

Status: integration complete; qualification NOTRUN until the full SHA is frozen and rebuilt. Source/binary hashes, raw failures and final results will be retained here and in the status index.
