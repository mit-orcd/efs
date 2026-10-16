# Intel buffered qualification — 2026-10-11 EDT

Human authorization in the AMD chat explicitly resumed a three-way test split after the earlier stop. Intel owns only the 48 buffered TCP first-stage invocations from the approved runner. Exact source branch is `release-v0.3.0-pre-alpha`, commit `28b2089be6a5df835cc7bb46b9c2a9e2f10df190`; private qualification branch is `qualification-intel-28b2089b`. No implementation change or shared branch movement occurred.

Both hosts independently verified all 21,738 tracked inputs against the frozen manifest and archive SHA256 `2ade76f798a2bbeac2f2647f6bd9b956a939a61126986deefc2c7c458e4f1454`. Fresh matching normal/fault builds of daemon, FUSE client, management tools and `libefs.a` passed on both hosts. AMD owns the full native/sanitizer evidence; these prerequisite builds do not duplicate or claim those suites on Intel.

Intel1 owns 26 normal scenarios, including core/peer POSIX. Intel2 owns 22 fault/worker/integration/Valgrind scenarios. Each fixture uses private stores, loopback ports and `/tmp` mounts. D25 remains inactive and experimental raft flags are OFF. Existing test assertions and timeouts are retained. No automatic failure retries, direct-I/O, poststage or RDMA scenarios belong to this report.

**Stopped at the user’s request on 2026-10-12 UTC.** No further scenarios launch. Final original assignment: 23 PASS, 24 NOTRUN. Intel1 stopped after seven scenarios; Intel2 finished its active namespace-max fault scenario and stopped after 17. Production is retained at its original source and mounts; immutable original tiny-file samples were verified before launch. The system screen socket directory has incorrect permissions; private user-owned SCREENDIR avoids a root change.

[Exact assignment inventory](../../results/measure/20261011-intel-buffered-28b2089b/intel-28b2089b-inventory.json) and [source proof](../../results/measure/20261011-intel-buffered-28b2089b/intel-28b2089b-local-source-proof.json) preserve the source and original approved commands. Adaptations affect only host paths, port allocations, disjoint scheduling and runner evidence/cleanup.


Both original controllers and the profiling observer retired. All private fixture processes/mounts and ports 34210–34213/34410–34413 are idle. Final source checks matched all 21,738 tracked files in both variants on both hosts. Production process identities, argv, environment, live and installed binary hashes, versions, and mounts exactly match their pre-launch baseline. All four production ports are reachable from each host. All 256 saved original tiny-file payload samples match from each host (512 checks). This does not claim a complete two-million-file inventory/content check. Production remains at `7bde5f432b6a6c069eeabaa6a753648c481ab4e5`.

Evidence is under the linked measurement directory: each host’s `final/` contains commands, binary identities, results, raw logs, per-case TSVs, stop request, and source/idle final proof; `production-stop.json` and `retirement-audit.json` preserve the final production audit. SHA manifests verify collected evidence. Remote private stores remain intact; node storage contents are excluded from the committed log copies. There are no scheduled relaunches. Native/sanitizer results owned by AMD and unrun Intel direct/RDMA/poststage scenarios are not claimed here.

Automatic approval review rejected notifying the AMD chat of this stop request, citing conflict with the earlier continue instruction. No notification workaround was attempted. This checkpoint documents the latest local user instruction and preserves the unresolved work for a future explicit resume.

| Host | Scenario | Status | Seconds |
|---|---|---|---|
| intel1_efs | gc-posix-buffered | PASS | 275.762 |
| intel1_efs | ordinary-overwrite-buffered | PASS | 18.104 |
| intel1_efs | query-buffered | PASS | 88.309 |
| intel1_efs | large-delta-buffered | PASS | 51.285 |
| intel1_efs | peer-mkdir-race-only-buffered | PASS | 48.136 |
| intel1_efs | peer-open-contention-only-buffered | PASS | 665.36 |
| intel1_efs | append-truncate-only-buffered | NOTRUN (user stop) | — |
| intel1_efs | mount-sync-buffered | NOTRUN (user stop) | — |
| intel1_efs | namespace-session-only-buffered | NOTRUN (user stop) | — |
| intel1_efs | applied-checkpoint-replay-only-buffered | NOTRUN (user stop) | — |
| intel1_efs | tiny-create-only-buffered | NOTRUN (user stop) | — |
| intel1_efs | tiny-mixed-only-buffered | NOTRUN (user stop) | — |
| intel1_efs | append-loaded-only-buffered | NOTRUN (user stop) | — |
| intel1_efs | efs-commands-only-buffered | NOTRUN (user stop) | — |
| intel1_efs | append-burst-buffered | NOTRUN (user stop) | — |
| intel1_efs | namespace-buffered | NOTRUN (user stop) | — |
| intel1_efs | integrity-buffered | NOTRUN (user stop) | — |
| intel1_efs | cache-buffered | NOTRUN (user stop) | — |
| intel1_efs | sessions-buffered | NOTRUN (user stop) | — |
| intel1_efs | create-buffered | NOTRUN (user stop) | — |
| intel1_efs | shutdown-buffered | NOTRUN (user stop) | — |
| intel1_efs | opid-restart-buffered | NOTRUN (user stop) | — |
| intel1_efs | opid-failover-buffered | NOTRUN (user stop) | — |
| intel1_efs | full-node-buffered | NOTRUN (user stop) | — |
| intel1_efs | two-roots-buffered | NOTRUN (user stop) | — |
| intel2_efs | create-retry-buffered | PASS | 70.983 |
| intel2_efs | create-adopt-negative-buffered | PASS | 70.473 |
| intel2_efs | create-adopt-reply-loss-buffered | PASS | 68.124 |
| intel2_efs | fuse-reply-failure-buffered | PASS | 63.16 |
| intel2_efs | put-barrier-buffered | PASS | 77.282 |
| intel2_efs | kv-wal-buffered | PASS | 17.656 |
| intel2_efs | transaction-apply-buffered | PASS | 69.929 |
| intel2_efs | compactor-buffered | PASS | 713.262 |
| intel2_efs | lookup-buffered | PASS | 64.905 |
| intel2_efs | put-reply-buffered | PASS | 64.459 |
| intel2_efs | storage-session-fence-buffered | PASS | 128.665 |
| intel2_efs | publication-pressure-buffered | PASS | 70.533 |
| intel2_efs | drain-pressure-buffered | PASS | 70.035 |
| intel2_efs | sync-write-fault-buffered | PASS | 135.342 |
| intel2_efs | stop-dirty-buffered | PASS | 98.687 |
| intel2_efs | namespace-boundary-fault-buffered | PASS | 126.302 |
| intel2_efs | namespace-max-fault-buffered | PASS | 235.667 |
| intel2_efs | worker-allocation-buffered | NOTRUN (user stop) | — |
| intel2_efs | worker-tls-buffered | NOTRUN (user stop) | — |
| intel2_efs | worker-growth-buffered | NOTRUN (user stop) | — |
| intel2_efs | fault-integration-buffered | NOTRUN (user stop) | — |
| intel2_efs | valgrind-buffered | NOTRUN (user stop) | — |
