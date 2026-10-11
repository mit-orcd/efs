# Retained-body flush mode simplification

October 11, 2026. S2 bounded private cleanup, applied after [incoming-pipe retry repair](flush-pipe-failure-20261011.md). Removes the constant-false `drop_body` parameter, per-job drop flags, stolen-body recovery branch and dead return-value contract from private pipeline helpers. Snapshots always retain their authoritative cache bodies through exact REPORT ACK; snapshot copies are freed by the pipeline. Production write.c is 26 physical lines smaller than the correctness checkpoint.

The old producer only wrote zero, and both production callers initialized/passed zero. Revalidated committed ORCD `795516914d433866e7c5d1f96c72a55f919f604e`, NUC `3538213cfe793dbdb9025c3e0f65cc96eb97149a` and MAC `b2c01fa05afa1287cd358f352ebfbc831b570e31` before cleanup. The old mode was manufactured only by the cache-memory harness. Its replacement test exercises actual retained-body failure: body remains owned/dirty, caller frees its distinct snapshot copy, and charge returns to one body.

No other recovery branches are removed: direct-flush concurrent replacement, failed merge-base fetch, re-dirtying, publication pins, exact ACK identity and active packed paths remain. The new full-pipe failure regression still re-dirties the unsubmitted incoming body. No public/native/wire API or server stats authority changes.

Focused gates pass: client-memory/read-pressure under ASan/UBSan; writer fence merge; fold observation; report pressure; append flush budget; complete write.c syntax check. Tests calling private helpers were updated to their new signatures and the now-unused fold harness pin stub was removed. Full merged Linux/mounted overlap, failed PUT/retry, exact REPORT ACK and refused teardown acceptance remain with release coordination.
