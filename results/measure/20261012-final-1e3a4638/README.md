# Final integrated source qualification — October 12, 2026

Frozen source: **1e3a46384e897a47751871b15e82ead86c9d0abd**, merge of reviewed qualification fixes into release-v0.3.0-pre-alpha. Later scope/operator/documentation commits do not modify src/include/tests/Makefile. Timing-only pack instrumentation is excluded. Root ran all work; no other agents assigned.

Required live scope: files engine, TCP, direct I/O, EFS_PACKED_WRITES=0, EFS_COMMANDS=0, EFS_COMMANDS_SERVER=0. Four private daemons with4G quota each, seven concurrent FUSE clients,216core cases/client,jobs16,original30s deadlines. Containers,packing,.efs,buffered,RDMA and dedicatedrepair/recovery excluded; incidental bundled native tests are not additional required qualification.

| Gate | Result |
| --- | --- |
| Normal build/full native | PASS |
| Fault build/full native | PASS |
| Early-ACK real Raft | PASS |
| Five affected ASan/UBSan extraction regressions | PASS |
| Seven-client core POSIX | FAIL:1505PASS/7FAIL of1512, no skips or notrun; dir_deep_nesting timed out30s on every client |
| Peer POSIX | PASS67/67 |
| Seven graceful client stops | PASS7/7 |
| Ordinary overwrite primary/peer/cold hashes | PASS4cases |
| Ordinary physical fragments and metadata queues drained | PASS |

Ordinary listing/names cases passed on all seven clients. Deep directory timeout remains an ordinary-files blocker even with packing disabled. No claim that excluding packing caused improvement: fresh fixture/launcher/workload differ from prior packing-specific run. Ordinary fixture completed71.638s including setup/peer/cleanup; isolated overwrite17.320s. Cold checks use controlled process crash/restart, not power-loss certification or repair acceptance.

All24,334regular tracked source files match before and after execution; normal/fault binary hashes retained. All89retrieved evidence files match independent remote SHA256 manifests. Reproduction scripts,commands,/proc runtime environments,TSVs,logs and failed evidence retained under native/,ordinary/,overwrite/. Private fixtures and mounts stopped; original unrelated services untouched. The frozen commit is **not release accepted**. No push/tag/main/devel promotion.
