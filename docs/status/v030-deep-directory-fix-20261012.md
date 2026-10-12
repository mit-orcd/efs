# AMD deep-directory timeout — fixed and regressed

Frozen release source: **4e239d029029b8105a6e07f0e69a26fdc449de32** on `release-v0.3.0-pre-alpha`. Root worked alone. No push, tag or shared branch promotion. Scope remains ordinary files/TCP/direct, `EFS_PACKED_WRITES=0`, `EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`; containers, packing, .efs and dedicated repair/recovery remain excluded.

## Finding and fix

Baseline 1e3a4638 failed `dir_deep_nesting` on all seven clients at the original 30-second deadline. A separate syscall observer showed most of that case's time in nested mkdir operations and their ancestor lookups. Existing RPC/read profiling showed parent authority fences dominate server lookup time, rather than key lookup. These are diagnostic observations, not per-case accounting for the whole matrix.

The host drained only eight admitted Raft frames before a durability/progress cycle. Increasing `HOST_INBOX_SLICE_MAX` from 8 to 32 amortizes those cycles under metadata load. The queue remains bounded at 256 frames, with existing byte limits, FIFO tail ordering, group receive bounds and post-KV durability/credit retirement unchanged. No lookup TTL, authority shortcut or weakened durability was introduced. A larger prefix trades some single-cycle latency for batching; the claim here is limited to the tested workload.

Discarded cross-group lookup routing experiment 91fb8ef0 still failed all seven deep cases. A one-frame slice experiment 0bd5fa01 also failed all seven and added two other failures on one client. Both were fully reverted on the isolated experiment branch; only the effective one-line change entered release as 4e239d02. The final tracked tree equals the passing 06f5420c experiment tree, but final release binaries were rebuilt and tested with their own commit identity.

## Quiet affected acceptance

Four AMD daemons, explicit **4G quota each**, seven clients each running the original **216-case core suite, jobs16**. No build or profiler overlapped quiet measurement. Original 30/75/180-second applicable budgets were preserved; suite wall duration is not a per-case deadline. Actual daemon/client environment records verify disabled features.

| Source/run | Core | Deep directory | Peer | Client stops |
| --- | --- | --- | --- | --- |
| 06f5420c first | 1512/1512 PASS | 7/7 PASS | 67/67 PASS | 7/7 PASS |
| 06f5420c repeat1 | 1512/1512 PASS | 7/7 PASS | 67/67 PASS | 7/7 PASS |
| 06f5420c repeat3 | 1512/1512 PASS | 7/7 PASS | 67/67 PASS | 7/7 PASS |
| **4e239d02 final release** | **1512/1512 PASS** | **7/7 PASS** | **67/67 PASS** | **7/7 PASS** |

Repeat2 was NOTRUN: the first daemon exited because port34900 was already in use. Its setup failure is retained; repeat3 used a fresh private port and fixture. It is not counted as a product pass or failure.

## Final exact-source checks

Normal build/full `make test`, early-ACK Raft, fault build/full `make EFS_FAULTS=1 test`: **PASS**. Five affected ASan/UBSan extractors (`host_inbox`, `readindex_operation`, `lookup_read_group`, `readindex_pair`, `readindex_remote_pair`): **PASS**. Full normal and fault suites took 91.2s and 90.9s respectively.

Final ordinary live fixture: **PASS**, 56.906s including setup/peer/stops. Isolated ordinary span-to-full overwrite primary/peer/fsync/cold exact-byte checks and physical fragment/queue reclamation: **PASS**, 17.475s. This focused cold test is not dedicated repair/recovery qualification; its wrapper deliberately records `acceptance:false` for broad acceptance.

All **24,466 tracked regular source inputs** matched before and after the final run; normal/fault binary hashes retained. Compact raw evidence initially verified against **383 independent remote SHA256 hashes**; controllers are additionally retained. Failed experiments and baseline diagnostics remain separate. All owned private processes/mounts are stopped; persistent AMD services were untouched.

[Raw evidence](../../results/measure/20261012-deep-directory-fix/README.md). The deep-directory blocker is closed for this release scope. Release publication/tagging remains a separate user decision; no excluded feature is qualified by these passes.
