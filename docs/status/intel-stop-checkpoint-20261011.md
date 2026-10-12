# Intel human-stop checkpoint — 2026-10-11 EDT

The human requested documentation, commit, and stop. Both Intel hosts are **IDLE** for this task. All owned test controllers are retired; no cleanup remains active and no further launches are queued. This checkpoint supersedes progress wording in earlier dated records. There is no automatic continuation.

## Source and workspace

Own worktree: `/Users/mike/Documents/Codex/2026-10-10/n-x20/work/efs-intel`, branch `devel-intel`, HEAD before this checkpoint `56c028efba25c6c0f3b7a2f7b7dfd7872f3a5237`. The checkpoint commit SHA is reported with the final handoff. DevOps worktree `work/devops-intel` is clean at `12508df70da5f661647d036828f03b9ae9e58e93`. Shared portal `/Users/mike/git/cluster` is on `devel` at `b102adc42aeeba0a4230b30b42d8908215fbc5a5`; its user-owned untracked `error.log` remains untouched.

Final frozen execution source is `410afed433b6da093bdc2acf37bec253ddbd586c`, corrected package v2: 1,563 exact Git input files and 52 additional directories. Archive SHA256: `e5685d586e1de93335d1ff339f22cb24ba64600949980b3386e1b07f556d732e`; inputs SHA256: `8939a4e679f5a431e4fb72a76248abc1867b5ec45a19fdf3d1e7c921f0b7bee6`; directory manifest SHA256: `272154ba5370b5c23d5defaef1e4f497c4be4fbc32daf1dea788f4d0809a3110`.

Local frozen tree is `work/efs-qualification-410afed4-v2`; each host retains `/data1/efs/intel-qualification-410afed4-v2/{normal,fault,logs}` and source/manifests. Build identities are `410afed4-release-normal` and planned `410afed4-release-fault`. Exact flags are retained in raw evidence; inherited `EFS_BUILD_TIME=2026-10-12T01:55:00Z` is not the actual execution timestamp. Planned mounted profile is TCP/direct and TCP/buffered, `.efs` enabled, D25 disabled, `EFS_RAFT_FLOW=0`, `EFS_RAFT_FLOW_EARLY_RETIRE=0`, `EFS_RAFT_GROUP_DRAIN=0`, with diagnostic traces unset. No final-source mounted gate launched.

The central reviewed source contains the fault-only CREATE correction; it has not been merged into this own branch. Reconciliation, deployment, push, tags and promotion are stopped.

## Actual final-source results

| Gate | Intel1 | Intel2 |
| --- | --- | --- |
| Normal build | PASS | PASS |
| Full normal Make-test, 245 recipes | PASS | PASS |
| Additional normal standalone scripts | 4/88 PASS; remainder STOPPED/NOTRUN | 6/88 PASS; remainder STOPPED/NOTRUN |
| Fault build and native suites | NOTRUN | NOTRUN |
| Focused sanitizer plan, 26 outer rows | NOTRUN | NOTRUN |
| Mounted matrix, 131 outer gates including concurrent POSIX and `.efs`, TCP/direct and TCP/buffered | NOTRUN | NOTRUN |

Only each already-executing native child was allowed to finish after stop; both returned actual exit 0. Intel1 parent/child `2203114`/`2210295` and Intel2 `2330253`/`2337457` are gone. Complete post-stop normal/fault input hashes and captured normal binary hashes match. Private process, mount and listener scans are empty, including ports 30810–30813, 30910–30913, 31010–31013 and 30231–30234.

Historical evidence stays separate: `c2bfb9e9` completed normal/fault native qualification (180 qualified outer rows per host) and 24 sanitizer raw rows per host. Four missing cluster-script-path failures were retained alongside four configured passing rechecks. Its live matrix recorded 7 PASS and 2 strict CREATE FAIL per host; remaining 122 rows are STOPPED/NOTRUN. The first 410 package failed normal Make-test because five committed documentation JSON inputs were omitted. Corrected v2 restores those exact inputs; original failures remain recorded.

## Production and payload preservation

Production remains `7bde5f432b6a6c069eeabaa6a753648c481ab4e5`, TCP/direct. Intel1 serves n1/n2 on ports 17432/17433 from `/data1/efs/n1` and `/data2/efs/n2`; Intel2 serves n3/n4 from `/data1/efs/n3` and `/data2/efs/n4`, each with 400 GiB quota. Each host retains only the original `/data1/efs/mnt` mount seeded against the opposite host. `.efs` is enabled. Exact production PIDs, arguments, versions, environments and five binary hashes match the pre-test snapshot. All four peer-port connection checks pass.

Original tiny-file jobs completed 1,000,000 files each with exit 0. Their original trees remain `/data1/efs/mnt/tiny-intel1-1m-20261010` and `/data1/efs/mnt/tiny-intel2-1m-20261010`. All 256 immutable sample paths match size and SHA256 from both hosts (512 checks). Full two-million-file inventory/content verification is NOTRUN. No replacement writers or trees were started.

## Blockers and deferred work

The c2 CREATE retry defect emitted two real 0/1/2 stage sequences for one operation. The reviewed central 410 change removes 27 duplicate fault-only lines and adds an integrated regression. Controlled native and sanitizer regression checks pass; strict fixed-source mounted CREATE, cold bytes, authoritative dentry and OPEN-token validation are NOTRUN. Early failed live assertions did not establish these ownership properties; original fixture PIDs were not recorded.

W100 concurrent POSIX/mixed EIO, W101 complete original two-million-file verification, W102 Intel2 peer/GC aggregation and W103 persistence/statistics timeouts remain open. W104 writable shared mmap returns ENODEV and is SKIP/deferred. W105 NFS stable handles and W106 RDMA validation remain open. RDMA belongs on AMD software RDMA or `orcd-test` hardware IB/RDMA. The fault contract retains 19 PASS and two named repair GAPs across 21 identities; GAP is not PASS. Externally reported Mac mounted timeouts remain unresolved and are not Intel qualification evidence.

After an explicit human resume: agree the common frozen source; finish fresh normal/fault/sanitizer prerequisites and strict CREATE/cold/token gates; run the complete concurrent/`.efs` TCP/direct and buffered matrix, canonical rounds, Valgrind and all fault identities; review actual fixes and reconcile branches; verify the complete original datasets and obtain a production window before deployment. No action is scheduled by this checkpoint.

## Evidence

[Machine-readable checkpoint](../../results/measure/20261011-intel-devel-intel/human-stop-20261011/checkpoint.json), [actual idle proof](../../results/measure/20261011-intel-devel-intel/human-stop-20261011/intel-human-stop-idle-proof.json), and [production preservation proof](../../results/measure/20261011-intel-devel-intel/human-stop-20261011/intel-human-stop-production-preservation.json) preserve source/configuration, gate dispositions and host state. Raw artifact manifests contain 34 Intel1 and 36 Intel2 SHA-verified files, including original failures, partial passes, build flags, binary identities and stop proofs.
