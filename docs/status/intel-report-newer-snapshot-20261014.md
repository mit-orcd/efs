# Private Intel older REPORT / newer PUT ownership — 2026-10-14

**Targeted regression and mounted reproduction PASS after correction;
broad qualification in progress; W100 remains OPEN.** Follow-up to
[operation-ID admission and span REPORT ownership](intel-opid-report-correctness-20261014.md).
Code checkpoint `5e3e8f3d260e86e9bb4d5c9f94c593f959292687` is committed;
this follow-up adds the ownership guard and its regressions.

## Failure and cause

During fresh isolated recovery/GC qualification of5e3e8f3d, Intel1 core failed
`perm_ftruncate_after_chmod` with EIO (215/216); Intel2 core216/216 passed.
Both isolated peer suites67/67 and subsequent fencing, physical reclamation,
restart, zero-quota and empty-inventory checks completed. Intel1's aggregate
stays FAIL; its controller continued independent gates and re-raised the original
POSIX failure. This is a stale-generation EIO, not proof of the original ORCD
REPORT BUSY EIO's cause.

A retained-file probe reproduced ftruncate EIO on each physical host:
2/500 cases each,16 workers,15.773/13.284 seconds. Diagnostic Intel1 client
with dcache tracing reproduced192/500 failures in43.711 seconds. Tracing
changes interleaving; these counts/durations are not a speed comparison.

The diagnostic trace for inode98441 shows:

- 14:59:44.161Z: original eight-byte PUT recorded at snapshot1.
- 14:59:44.168Z: newer full truncate-tail snapshot4 clears `dirty`, while
 `object_seq` still names snapshot1's completed PUT.
- 14:59:44.172Z: the older REPORT succeeds. Completion checks the old
 object identity plus `!dirty`, then clears ranges/pin and frees the body.
- 14:59:44.175Z: snapshot4's PUT completes, but its cache body has gone.
 The STALE classifier repeatedly reports an uncommitted object; replay has
  no body and cannot rematerialize it. The application receives EIO.

`dcache_note_committed`'s completion ownership test was unchanged by5e3e8f3d;
this is a separate pre-existing gap exposed by further qualification. A snapshot
clears dirty before its PUT finishes. Matching the last completed object is
therefore insufficient authority to release the current body.

The correction releases ranges, eviction pin and body only if the matching
REPORT also covers the newest captured snapshot (`snap_seq <= object_seq`).
The older publication may be acknowledged while the newer snapshot remains
owned. No receipt-window, retry/deadline, quota, storage-format or compression
change is involved. Committing the newer snapshot releases its body normally.

## Source, profile and targeted qualification

Parent5e3e8f3d plus the small `write.c` guard, extended
`tests/test_client_memory.py`, and `tests/live/chmod_truncate.py`.
Build label `5e3e8f3d-fix2`, frozen directory
`/data1/efs/w100-private/devel-opid-fix2` on both hosts. Normal GCC
`-O3 -g -fno-omit-frame-pointer -march=native -mtune=native` builds.

Same four-node private two-host Intel fixture/16 GiB per-node quotas as the
linked report: ports27432/27433, files engine/direct I/O, TCP;
`.efs` client/server, packing, containers and expiry disabled. The corrected
four daemons/two main clients reuse the same stores; production is untouched.
Normal live trace is optional operation-ID logging; a separate owned client
uses dcache tracing. Diagnostic clients are drained after their workloads.

The memory regression executes the production completion function against a
newer in-flight snapshot with the older object still installed. It requires the
new body's bytes, ranges and eviction pin to survive the old ACK and cache
reclamation, excludes another late old ACK after the newer PUT lands, then
requires exact newer ACK cleanup. Normal and ASan/UBSan/leak runs PASS on both
Intel hosts. Selecting previous production source compiles and executes, then
fails precisely at the retained-body assertion on both hosts.

Mounted fixed runs (all16 workers, no application retries):
Intel1 1000/1000 and Intel2 1000/1000 PASS in29.455/29.778 seconds;
separate traced Intel1 client500/500 PASS in16.631 seconds. These overlap
native qualification and each other; they are correctness/load evidence, not
quiet performance measurements. Every accepted file has exact EOF3 and `ABC`.

## Reproduction

Existing private source/build/SSH/FUSE prerequisites from the linked report.
Preserve original limits; do not replace a failed verdict with a passing repeat.
From each candidate source tree on Intel:

```sh
python3 tests/test_client_memory.py
ASAN_OPTIONS=detect_leaks=1 python3 tests/test_client_memory.py --sanitize
EFS_CLIENT_MEMORY_WRITE_SOURCE=/old/src/client/write.c python3 tests/test_client_memory.py
python3 tests/live/chmod_truncate.py /PRIVATE/MOUNT --files 1000 --workers 16
```

The old-source control must execute and fail its assertion, not fail compilation.
The mounted probe keeps every attempted file. Its sequence is exclusive create,
write `ABCDEFGH`, chmod0444, ftruncate3 through the existing descriptor, close,
chmod0644, exact stat/read. On failure it records the phase and close error;
it never retries an application operation or closes a recycled descriptor.
For a successful fixture after a cold restart:

```sh
python3 tests/live/chmod_truncate.py /PRIVATE/MOUNT --files 1000 --workers 16 --verify /PRIVATE/MOUNT/chmod-truncate-UNIQUE
```

## Evidence and baseline cleanup

Raw output outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-opid-report-20261014/`.
Old native control logs are `newer-snapshot-old-negative-intel{1,2}_efs.log`;
trace is `chmod-dcache-trace.log`; mounted outcomes are
`chmod-truncate-{probe,fixed}*`; source archive is `fix2-source.tar`.
Original isolated fixtures: Intel1 `/data1/efs/gc-direct-n9n_zi12`,
Intel2 `/data1/efs/gc-direct-5e8lx9gp` (all owned processes stopped).

The old diagnostic client correctly refused drain because the demonstrated
missing bodies left unresolved publications. Before cleanup, an independent
fixed client saved **all1500 baseline probe files**, full published bytes,
inode/mode/size/SHA256 manifest, outside EFS at Intel1
`/data1/efs/w100-private/results/chmod-baseline-saved-20261014`.
Only those three uniquely owned probe directories were then unlinked normally;
the old clients drained cleanly. No force-discard, mkfs, production shutdown
or removal of other fixtures was used. Failed published EOFs/bytes remain in
that external archive for inspection.
