# AMD complete-suite qualification — October 11, 2026

Human request: run all test suites on AMD and report all issues. This resumes AMD testing only; other owners remain stopped. The initial checkpoint authorized testing only. Subsequent human authorization now permits coordinated packing/repair fixes, integration into the release branch and affected reruns; promotion, tag and push remain unauthorized.

Frozen source: `28b2089be6a5df835cc7bb46b9c2a9e2f10df190`. Complete tracked-file archive: 21,738 files, SHA-256 `2ade76f798a2bbeac2f2647f6bd9b956a939a61126986deefc2c7c458e4f1454`. Every input was hash-verified on AMD; 52 referenced directory-only evidence paths were recreated.

Isolated source `/data1/efs/release-v030-28b2089b`; evidence `/data1/efs/release-v030-results-20261012-28b2089b`. Remote UTC naming differs from the operator's October 11 date. Current screens `amd-all-approved-28b2089b` and `amd-all-extra-readyfinal-28b2089b`; earlier native/build screens completed. Installed source, four original daemon stores and mounted user data remain separate. D25 and experimental flow/group-drain flags remain disabled.

## Execution ledger

**Current checkpoint: STOPPED.** Automatic approval review reported a newer human instruction to document, commit and stop all chats, and rejected further Intel coordination. No new tests or implementation work are launched. AMD owned fixtures/screens are gone and both original mounts are restored. Intel is checkpointing; NUC was notified to stop launches and restore service. The completed and incomplete evidence below remains source-specific.

Before stop: Full normal and fault-enabled Make suites PASS (242 Python recipe executions each, plus native C binaries and architecture gates). Twenty focused sanitizer regressions and two fully sanitized real-LSM tests PASS; integrated CREATE and common CREATE fault sanitizers PASS. Nine standalone root Python regressions PASS in both requested environments; the separately defined FUSE worker test is queued. Direct TCP GC/POSIX and ordinary overwrite cold gates PASS. Direct integrity, cache, session/lease cleanup and CREATE lifecycle gates also PASS. Shutdown/recovery is running; other live verdicts remain pending. A launch or queued case is not a pass. Every independent follow-on gate records its exact command, source, duration, exit code and log in `all-gates.jsonl`.

Coverage includes normal/fault native, standalone regressions, ASan/UBSan and real-LSM fixtures; buffered/direct TCP POSIX, peer, extras, packing, query and private crash/recovery/fault cases; Valgrind; same-host software RDMA. Hardware GPU-direct performance, multi-host ORCD orchestration and destructive raw-device performance scripts are outside this single-host functional qualification. Any unexecuted applicable case must be listed explicitly in the final report.

Repair GAP results from the fault harness remain gaps even if readable bytes survive. Original per-case timeouts and assertions remain unchanged. Independent cases continue after a failure; failed prerequisites must not produce passing dependent verdicts.

## Issues already observed

- Both native builds warn that `host_append_complete_batched` is defined but unused in `src/server/raft_host.c:12969`. This is a cleanup warning, not a native-suite failure.
- Initial live fixtures under arbitrary `/data1` scratch mount paths failed with `fusermount3: mount failed: Permission denied`. Original logs remain; approved `/data1/efs/mnt` and `mnt2` are borrowed for corrected execution. These initial failures are setup failures, not evidence of an I/O assertion failure.
- The simulator reports `repair_restores_protection` GAP: no fragment rebuild after a lost/damaged member. Its exit 2 is not a full pass.
- Approved direct seven-client packing: all seven clients report 209 PASS, 4 FAIL, 1 SKIP and 7 NOTRUN of 221. Failures include `dir_deep_nesting` and `virt_stats_readable` at the original 30-second budgets, `dir_many_files` killed by signal 15, and `virt_find_query` blocked behind a surviving stats worker. Clean extra-client teardown refused at the first extra mount. This fixture did not reach its later cold/physical acceptance; those remain NOTRUN for this run. A two-job fault-library prerequisite build overlapped part of the high-concurrency run, so this run alone is not an isolated performance comparison.
- AMD has no active NFS server/export configuration, nor IOR/MPI/ecrawl executables in PATH. The original-shape Mac NFS W55 acceptance remains NOTRUN; same-host FUSE does not close it. External-tool capacity/performance gates are not silently counted as passes.

The native logs contain 65 C-binary recipe executions (63 distinct binaries) and 242 Python/document recipes per build, with no native SKIP verdict. The two deliberate full source builds warn about the same unused function. AMD environment: Ubuntu kernel `7.0.0-38-generic`, GCC 15.2.0, Python 3.14.4.

Additional direct results: 216/216 core POSIX and 67/67 concurrent POSIX PASS; large-delta 1,025-entry, competing mkdir, peer OPEN contention, append/truncate, mount sync, namespace-session, tiny CREATE/mixed load, enabled command trees, 1,000-record append burst and comprehensive namespace gates PASS.

Loaded append/GC completed four rounds of 800 exact warm/peer/cold records, then round 5 failed with `EBUSY` in `shutil.rmtree(loaded-garbage-04)`. Remaining rounds 6–20 are NOTRUN, and the fifth round's later exact/cold acceptance did not execute. No earlier pass certifies those missing rounds.

The default applied-checkpoint regression failed because its old `applied.N` interposer did not execute within 30 seconds. The merged host documents that a durable, validated atomic KV cursor avoids another redundant `applied.N` fsync. The fixture already supports `EFS_TEST_CURSOR_CHECKPOINT=1`; that separate, existing cold-cursor oracle is queued. This is a stale default test expectation until the cursor-aware gate establishes recovery; it is not evidence that acknowledged bytes were lost.

Progress at 2026-10-12 03:25 UTC: approximately 140 remaining test invocations, including persistence and storage-engine phase checks, plus fixture setup/cleanup. This is a planning count, not a claim of passing coverage. Direct session and CREATE gates completed; shutdown/recovery is active. The first matrix is followed by cursor-aware recovery, query policy/load, overload feedback, publication crash gates, files/containers persistence and engine A/B checks.

At 03:26 UTC, direct shutdown/WAL-tail recovery PASS: repaired invalid tails, later fsynced files survived a second cold crash, orphan/GC queues drained and physical inventory remained empty. Operation-ID restart is next.

## Authorized three-cluster split — October 11, 2026

The human authorized messaging Intel and NUC and dividing qualification work across the three owners. All verdicts must identify frozen source branch `release-v0.3.0-pre-alpha`, exact commit `28b2089be6a5df835cc7bb46b9c2a9e2f10df190`. Private qualification branches may differ in name; implementation inputs must not. Shared branch tips, implementation, tag and push remain unchanged.

| Owner | Assigned remaining coverage | Execution |
|---|---|---|
| AMD / this chat | Remaining direct TCP first-stage scenarios and Valgrind; software RXE; unpaced overload/replication both I/O modes; consolidated report | Active controller drains its current child and preserves actual wait status before resuming only assigned cases |
| Intel / efs-coding 5 - intel | Full 48-scenario buffered TCP first-stage matrix, including seven-client packing, fault integration and Valgrind | Assignment accepted; private frozen-source fixture preparation |
| NUC / efs-coding 3 - nuc | Additional recovery/query/pressure checks and files/containers × direct/buffered persistence, engine A/B, container POSIX/peer and extras | Assignment accepted; private frozen-source fixture preparation |

This divides remaining work approximately by runtime, not equal test counts. Completed AMD native/sanitizer/regression evidence is retained. Packing and overload run without competing local builds; each cluster has separate stores, ports and mounts. Intel/NUC supply their own environment and runtime identity. Cross-cluster throughput is not an engine A/B comparison. A host-specific pass cannot erase the earlier AMD packing timeout or loaded-delete failure.

Direct parent `create`, `namespace` and `integrity` combinations PASS, covering lease, boundary/max/fresh and append-replay cases; NUC need not repeat those conditional direct cases. Buffered conditional coverage waits for Intel parent verdicts. Other owners' historical STOP records remain valid except the explicitly authorized Intel/NUC assignments above.

NUC checkpoint: exact source-input verification and matching normal/fault prerequisite builds PASS. Runtime SHA/version manifests and original service/payload before-proof retained. Private poststage screen launched; normal extra-full remains normal after confirming only explicit `--worker-fault` needs the fault client. NUC restoration remains required; launch is not a test PASS.

Intel checkpoint: all 21,738 archive inputs verified against exact frozen Git source and manifest. Matching normal/fault prerequisite builds, including libefs.a, PASS on both hosts. Detached controllers own exactly 48 buffered scenarios (26 normal on Intel1; 22 fault/worker/integration/Valgrind on Intel2), with private stores, /tmp mounts and independently allocated ports. Production remains running and payload hashes are baselined. Tests are running, not yet qualified; buffered parent verdicts will determine conditional NUC coverage.

Intel `create-retry-buffered` PASS (actual exit 0, 70.983 seconds) at frozen28: actual FUSE lost committed reply and BUSY replay retained one operation ID/inode, strict stages, cold bytes and deletion cleanup. Raw remote evidence: `/data1/efs/intel-qual-28b2089b/evidence/gates/create-retry-buffered/{command.json,result.json,run.log}`. This scenario supersedes the earlier duplicate-hook symptom for its tested buffered scope only; full CREATE parent combination and candidate acceptance remain pending. Evidence retrieval/central hash verification is still pending.

Intel buffered baseline `gc-posix-buffered` PASS, 216/216 core and 67/67 peer, 275.762 seconds. Concurrent seven-client/jobs16 packing reproduces original-budget timeouts with no competing builds/test workload on Intel1 (production unchanged). Client 0: 203 PASS / 17 FAIL / 1 SKIP / 0 NOTRUN of 221. Other client summaries: 171/18/1/31, 166/18/1/36, 166/21/1/33, 170/18/1/32, 170/18/1/32, 169/18/1/33 (PASS/FAIL/SKIP/NOTRUN). Failures include readdir/deep names/random contents/concurrent create+append/virtual stats/find; original 30-second budgets except dir_many_files 75 seconds. mmap ENODEV SKIP retained. Peer/cold/cleanup still running: no final aggregate verdict yet. This eliminates overlapping prerequisite builds as a necessary explanation for this separate Intel reproduction; it does not establish a root cause or remove the AMD failures. Remote evidence: `/data1/efs/intel-qual-28b2089b/evidence/gates/packing-seven-buffered/command.json` and `evidence/live/packing-seven-buffered/gc-buffered-1r8kgk9s/posix-client-{0..6}.{log,tsv}`; central retrieval/hash verification pending.

AMD remaining direct worker allocation/TLS/growth fault cases PASS. Full direct fault harness returns exit 2 with **19 PASS and 2 GAP**, not an assertion FAIL: simulation and real integration both record `repair_restores_protection` GAP. Node-loss/restart, corrupt-fragment reconstruction, missing-checksum rejection and crash-after-fsync readable bytes PASS, but damaged/lost fragments are not rebuilt and protection is not restored. Raw `/tmp/efs-all-28b2089b-fault-direct/results.jsonl`. Wrapper exit-based FAIL label must be interpreted with these case-level GAP verdicts. Direct Valgrind now running.

## Authorized repair phase

Human requested continued tests followed by coordinated packing-timeout and fragment-repair fixes, release-branch integration and affected reruns. Intel accepted packing ownership on a separate frozen28-based fix worktree; NUC accepted repair ownership similarly. AMD owns review/integration and affected reruns. Original frozen-source evidence remains separate; tests on a fixed commit cannot inherit unchanged-source identity silently.

AMD direct Valgrind exited 2 after native AVX-512 instructions were unrecognized by Valgrind 3.26.0 in query tests and the daemon. Abrupt SIGILL leak summaries are not clean-exit leak proof; compatible-build retry is required to distinguish genuine leaks. Original logs are retained. RXE preflight exited 1 at `tests/test_rdma_xprt.c:126 server_accept`; the CQ moderation warning preceded it but is nonfatal by source inspection and is not established as the cause. No mounted RDMA cases ran. Both are outstanding qualification issues.

Frozen-source AMD compact evidence snapshot retrieved and SHA-256 verified locally: 1123 files under `results/measure/20261011-amd-all-suites/28b2089b`. Ongoing runs require a final refresh; this interim snapshot does not certify completion. Compatible Valgrind rebuild is queued behind exclusive overload, using forced recompilation and explicit supported ISA flags; assertions/leak criteria remain unchanged.

Intel original buffered packing final verdict FAIL (exit 1, 434.778 seconds): peer 67/67 and cold acknowledged packed/ordinary bytes PASS, but original 180-second physical packed reclamation oracle failed after POSIX timeout failures. Fixture cleanup completed and independent queue continued. NUC repair review confirms W74 missing production reconstructor/debt/owner workflow, rather than stale GAP labels; staged safe implementation review is ongoing and no repair closure is claimed.

AMD unpaced direct overload/replication PASS: refused I/O remains visible, acknowledged bytes survived cold restart, physical deletion drained and post-pressure cold write/reclamation passed. Buffered counterpart is active.

RXE setup review found the earlier AMD acceptance explicitly used `EFS_RDMA_GID_INDEX=1` (IPv4 GID), whereas this initial preflight selected default index 0 (link-local IPv6). A distinct unchanged-source `rdma-posix-gid1` retry is queued after overload and compatible Valgrind; original server_accept failure remains retained. This is a configuration hypothesis until the retry completes.

AMD buffered unpaced overload/replication also PASS, including cold acknowledged bytes, post-pressure cold write and physical reclamation. Both borrowed production mounts restored successfully. Compatible Valgrind forced rebuild has started in a separate source tree; original source and installed daemon stores remain untouched.

AMD compatible-build direct Valgrind PASS (exit 0, current C unit suite, efsd workload and mounted efs-fuse mini-POSIX). Query-delta/disk-I/O and daemon now complete with zero definite/indirect leaks and zero gated access/uninitialised errors. Native AVX-512 SIGILL remains an original tool/build incompatibility verdict; abrupt leak summaries were not confirmed on clean compatible exit. RDMA memory phase explicitly SKIP because this run selected TCP; separate RXE functional retry does not substitute for RDMA memory instrumentation.

Explicit IPv4-GID RXE filesystem retry `rdma-posix-gid1` PASS (exit 0), including 67/67 peer POSIX and strict RDMA-up checks; original preflight failure persists as documented incorrect GID selection. Owned private mounts cleaned. Separate compatible Valgrind RDMA phase now running with auto server/strict RDMA client, IPv4 address/GID and previously passed unchanged-unit results reused through the existing skip-units option. No RDMA memory PASS until real upgrade/workload/leak logs are checked.

Compatible RDMA Valgrind PASS with actual transport-up/workload evidence and zero definite/indirect leaks on client and server. Existing RDMA phase intentionally gates leaks only (ibverbs/DMA uninitialised/access reports excluded by its original criteria); unchanged unit verdicts reused, not rerun. IPv4-GID functional retry and memory checks resolve the initial setup/tool blockers for their stated scopes.

AMD separate exclusive profiled direct seven-client/jobs16 diagnostic launched on frozen28 with original budgets, retained normal binaries, phase/RPC/read/Raft/query observation hooks. It is diagnostic, not quiet fixed-source acceptance; original failures remain retained. Installed mount restoration is queued in its finally block.

First profiled diagnostic launch was refused because previously restored borrowed mounts were still active; no packing workload ran. Duplicate restore attempts reported the existing mount lock, and original mounts remained intact. Cleanly drained both original mounts, then launched distinct `packing-seven-direct-profiled-approved`; original setup attempt retained separately. Added bounded private-only worker wait/FD samples.

Exclusive profiled AMD direct packing reproduces all seven clients at 209 PASS / 4 FAIL / 1 SKIP / 7 NOTRUN (~44.5 seconds). Extra-client stop refused due to surviving timed-out workers; fixture later peer/cold/physical oracles NOTRUN. Original mounts restored PASS. Primary RPC checkout cost is negligible (18,081us vs132,329,235us receive aggregate,23,112calls); deep-case phases reached leaf access and recursive cleanup before timeout. Query source capture15ms versus build8,948ms on one published cut. These are diagnostic measurements, not a root-cause or fixed-source acceptance claim. Compact evidence refreshed and verified: 1354 files.

Full phase analysis clarifies deep creation consumed about 28.9–29.7 seconds for four clients, leaving under a second for leaf access/cleanup; three clients never reached the leaf. This does not prove a cleanup deadlock. Server lookup averages are dominated by read authority: node2 lookup5,985us vsparent authority5,902us and KV39us. Any optimization must preserve request freshness; no stale cache/read-proof reuse is authorized merely by the profile.

## Stop checkpoint and remaining implementation

AMD verification: no screen sockets and no owned release-test processes remain; original `/data1/efs/mnt` and `mnt2` are mounted. Original daemon PIDs1072884/1072921/1072964/1073011 were verified unchanged after qualification. No production source/store replacement, push, tag or branch promotion occurred.

Release integration commits: `b2e2ca7b` frozen qualification evidence/coordination; `5d6b61cc` and `77d55fb4` repair design/fencing investigation; `9d2a9090` completed overload/memory/RDMA and exclusive packing diagnosis. These contain documentation/evidence only. No packing or fragment-repair implementation fix has landed, and no fixed-source affected acceptance has run.

Packing remains reproducibly failing on AMD direct and Intel buffered. Intel buffered also failed physical reclamation within180 seconds despite peer/cold-byte passes. Repair remains missing W74 functionality; safe initial same-target work needs durable job ownership/target revocation coupled to generation GC. NUC's uncommitted inactive journal prerequisite was under review: checked digest accumulation, generic command receipt refusal and business-verdict Raft progress must be resolved before integration. It cannot be described as restored protection.

Additional frozen NUC issue: container/direct core `mtime_monotonic_many_writes` timeout at unchanged30 seconds. Container/direct persistence and all five engine A/B phases passed before that core failure; final totals pending owner checkpoint. Small staged test builds overlapped part of NUC qualification and are retained as context, not a cause claim.

Remaining Intel/NUC matrix cases must be recorded as stopped/notrun by their final ledgers. Original Mac NFS, large-capacity and external-tool obligations remain notrun. A future explicit resume must re-confirm the candidate, finish reviewed implementation, integrate logical fixes and rerun affected original-deadline tests; no automatic continuation is scheduled.

NUC final owner stop checkpoint received: qualification `68901f73` on private qualification-nuc-28b2089b preserves unchanged frozen28 inputs. Container/direct core **209 PASS / 7 FAIL / 0 SKIP of216** under original budgets. Peer was interrupted by controlled stop; remaining buffered-engine cases and W97 slowdown comparison NOTRUN. Private daemons/mounts cleaned; original startup required retained-process idempotent follow-up, whose exit0 after-proof verifies five installed images, four mapped daemons, two mounts and36 payload samples. Repair WIP `54e0f5d7` preserves inactive journal, four blocking review issues, WAL-shim failure and no restored-protection oracle; explicitly **not accepted, integrated or activated**. NUC has stopped and restored service.
