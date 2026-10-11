# NUC W52/W97 continuation evidence

Current installed engine: f52cd2dd-nuc. Both placement matrices use the earlier frozen accepted 108afa3e-nuc engine. Earlier staged W52 core: 4afc063c; later 64-lane test-only overlay f54eff33.

`resume/` retains the completed five-minute eight-worker create/write/close run, phase windows, physical counters, early/late perf text and syscall summaries. Binary perf recordings remain on NUC. This is instrumented workload evidence with sampled warm peer verification, not a cold or power-loss gate.

Focused staged W52 native transaction/recovery and actual host-loop outputs are retained here; the original GCC test-only warning is retained beside its corrected pass. The original bounded-batch native suite passes; its direct fixture timed out and was not installed. The later f52cd2dd native and both full direct/buffered live suites pass; restoration and qualified installation pass; the separate preserved-store long/cold gates remain pending. `meta-placement/` retains all three completed ten-minute placement cells, 64 cold samples per cell, sync counters, Raft observations, physical counter samples, catch-up and successful regular-service restoration. The summary preserves measurement limits.

Remote root: /data1/efs/devel-nuc-20261011. See docs/status/nuc-w52-w97-resume-20261011.md for scope and checkpoints.

`data-placement/` retains the accepted SATA → NVMe → SATA data control,
always with SATA metadata and the same 108afa3e engine. The last cold startup
initially rejects an unsafe fixture quota; `data-cold-quota-retry/` records the
60 GiB cold-only retry, preserved timed data and successful regular restoration.
All cells have 64 exact cold payload checks. Neither matrix identifies NAND,
cache or thermal causality or establishes a hardware ceiling.

`root-scope-focused/` retains the Linux production PUT regression for 19cbb11f
plus packed-fence caller correction 766814c8. The corrected run passes; old
scope fails its directory-count assertion. The original compile failure is
retained separately. This candidate is not installed or performance-accepted.
The frozen f52cd2dd append candidate is separate; its full acceptance chain
has passed full native and both direct/buffered live recipes after the retained doc-sidecar fixture failures. Restoration and qualified installation pass. Preserved-store long/cold gates remain guarded and pending.

`clock-direct-live/` retains the accepted full direct fixture, including 2k
exact peer/cold bytes; its controller log/exit are retained under `clock-live/`.
`clock-direct-profile/` contains the separate CPU and path-resolved sync window
plus coverage-aware reduction. No matched performance or hardware-ceiling
claim follows from instrumentation.

`clock-buffered-live/` retains the complete buffered fixture and its exact
2k peer/cold bytes. Both mode exits are recorded in `clock-live/`; its separate
restoration marker must also pass before installation.

`clock-install-initial-readiness-failure/` retains the initial EBUSY and backup
manifest; the actual rollback archive remains remote. `clock-install/` and
`clock-install-readiness-retry/` retain unchanged exact images, four-daemon/two-
client identity and all 32 preserved-payload checks after actual readiness.
`clock-readiness-blocked/` retains the explicit not-run dependent outcomes;
new queued PIDs/script hashes are in the retry's queued.json.

`clock-preserved-short/` passes the installed-engine 2k append/fsync and exact
warm two-mount byte gate. It is separate from both private mode fixtures;
its normal cold restart and the subsequent long workload remain pending.
