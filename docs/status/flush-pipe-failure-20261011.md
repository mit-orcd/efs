# Full flush-pipeline failure retains retry eligibility

October 11, 2026. New correctness finding while reviewing S2 constant-false body-drop mode. Fixed separately before mode cleanup.

## Trigger and fix

A snapshot for the next chunk is already copied and its dirty bit cleared before `flush_pipe_add` notices that the 32-job pipe is full. If draining those queued jobs fails, the incoming job never reaches PUT. Previously the error branch invoked `dcache_flush_keep` only when `drop_body` was true. Production always supplies false, so it freed the incoming snapshot and closed its window without re-dirtying the retained local body. That chunk could be skipped by a subsequent dirty-set retry even though its bytes remained in the cache.

The failure branch now always invokes the existing failed-PUT retention helper with the actual ownership mode before freeing the snapshot. Current retained bodies are re-dirtied/pinned in place; legacy stolen bodies, while that mode still exists, retain the existing ownership return behavior. The returned pipeline error and already queued jobs are unchanged. No public API, wire/native format, authoritative stats or packed consumer is removed.

## Reproduction and validation

Extended `test_client_memory.py` extracts the actual chunk-job/pipe structures and init/drain/add helpers alongside the existing actual cache ownership helpers. A stubbed PUT pool fails a full pipe while a 33rd clean snapshot is being admitted. The test asserts all 33 local bodies are dirty/on the retry set with unchanged marker bytes, their snapshots are freed, and all windows close. Dirty accounting is compared with the harness's prior reading so unrelated earlier manufactured states do not confound the regression.

Against original `95289a76` write source, the final regression fails its dirty-accounting assertion (the incoming body is omitted). With the focused fix, ordinary and ASan/UBSan client-memory/read-pressure gates pass; writer fence merge and fold observation pass; full write.c syntax check passes. The test supports `EFS_CLIENT_MEMORY_WRITE_SOURCE` for exact before/after replay. RPCs are stubbed; this is not proof of an observed mounted data-loss incident.

Affected `flush_pipe_add`, `flush_pipe_drain`, and `dcache_flush_keep` bodies match the inspected original baseline in ORCD `795516914d433866e7c5d1f96c72a55f919f604e`, NUC `3538213cfe793dbdb9025c3e0f65cc96eb97149a`, and MAC `b2c01fa05afa1287cd358f352ebfbc831b570e31`. Integrate this correctness commit before S2 mode simplification. Full merged Linux/FUSE, failed transport/PUT then retry, exact REPORT ACK and mounted hash acceptance remain with release coordination.
