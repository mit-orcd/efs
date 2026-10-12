# Preserved worktree diagnostics — 2026-10-12

This review preserves 39 unique AMD diagnostic artifacts at their original repository paths and all nine W92 handoff logs/completion markers. The manifest records source worktree, Git blob, SHA-256 and preservation decision for all 69 reviewed files. The 21 omitted AMD duplicates already exist byte-for-byte in release history.

AMD evidence includes partial pressure captures and packet traces, not new completed qualification results. Completed comparison conclusions remain in the AMD feedback integration ledger and final comparison evidence. Partial captures must not be counted as new passes or failures.

W92's four capacity cold-handoff attempts finished with exit 1 and `writer state 1: cold handoff refused`; the completion marker is dated 2026-10-11 04:05:07 UTC. SSH banner/time-out failures are retained in the logs. This records refused handoffs, not successful cold recovery or proof of data loss. W92 remains open and its `.efs` functionality is excluded from this alpha release.

No production code changes. Source leftovers may be cleaned after this evidence is committed and verified.
