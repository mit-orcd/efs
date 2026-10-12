# v0.3.1 TCP/buffered ordinary-files qualification

User requested a buffered rerun on AMD with no file packing, containers or .efs. Root worked alone on `release-v0.3.1-pre-alpha`; no source change, push, tag or main/devel promotion. Exact frozen tested source remains **15e2a5310df1dd0fd4e307a9508c95d513d14672**. Current branch product/test tree is identical to that source. This adds buffered-mode validation; it does not activate excluded features or experimental repair.

## Results

| Gate | Result |
| --- | --- |
| Normal build/full native | PASS; native92.6s |
| Fault build/full native | PASS; native91.6s |
| Early-ACK Raft | PASS |
| Eight affected ASan/UBSan extractors | PASS |
| Fully instrumented Raft native/early-ACK | PASS |
| Seven concurrent clients,216 core cases each | **1512/1512 PASS**, zero skips/notrun |
| Deep-directory original30-second deadline | **7/7 PASS** |
| Two-client peer cases | **67/67 PASS** |
| Graceful client stops | **7/7 PASS** |
| Ordinary overwrite primary/peer/fsync/cold exact bytes | PASS |
| Physical fragments and metadata queues drained | PASS |

Four daemons, explicit4G quota each, seven clients/jobs16. Original applicable30/75/180-second budgets unchanged. Quiet live fixture56.221s including setup, suites, peer and shutdown; isolated overwrite/cold/reclamation17.293s. These are qualification durations, not controlled throughput benchmarks or evidence of a general speedup over direct I/O. The cold test is controlled restart validation, not dedicated repair/recovery; its external wrapper retains `acceptance:false` for broader acceptance.

All server startup logs verify **`storage-engine=files direct_io=off`**, and the concurrent fixture explicitly uses `--storage-engine files --no-direct-io`. Actual process environment records verify TCP, `EFS_PACKED_WRITES=0`, client `EFS_COMMANDS=0`, daemon `EFS_COMMANDS_SERVER=0`, and experimentalrepair0. No profiler/build overlapped live measurement. All28,065 regular tracked source inputs matched. Binary hashes and **127 independent remote evidence hashes** retained. All owned private fixtures stopped; unrelated AMD services untouched.

## Preserved setup failures and cleanup

Initial live setup NOTRUN: explicit4G quota exceeded safe maximum2.99G on the shared filesystem. Initial overwrite setup also failed before tests. Completed private fault/sanitizer build copies were removed after retaining binary hashes, restoring headroom without weakening the quota or reserve.

Fresh live retry1 passed all cases. Its retained private stores then exhausted safe headroom for overwrite retry1, which also failed before tests. After preserving live results/logs and verifying client stops and no owned daemon processes, only those four completed private stores were erased. Fresh overwrite retry2 passed. Both sets of startup refusals are retained as setup failures; they are not hidden, counted as product passes or attributed to buffered-mode correctness. No quota/reserve changes were made.

[Raw evidence](../../results/measure/20261012-v031-buffered-15e2a531/README.md). [Same-source direct-I/O report](v031-qualification-20261012.md). Packing, containers, .efs, RDMA and dedicated repair/recovery remain unqualified by this rerun.
