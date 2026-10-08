# AMD replay and persistence checkpoint — Oct 8, 2026

Partial package evidence, **not production activation approval**. Functional
runs use direct I/O/TCP, one AMD host/four daemons. No efs-bench changes.

| Evidence | Outcome |
| --- | --- |
| `replay/` | Exact durable replay with lost replies, 32 concurrent repeats, explicit group-0 leader death, ACK retirement, payload refusal, POSIX 216 pass / 0 fail / 1 skip, peer 64/64, exact 800-record cold audit and physical GC/restart. |
| `persistence/` | Same broad gates with production PUT file/pathname persistence, plus active shutdown. Actual store buffered/direct/bounce fault tests also pass. |
| `append-transaction-tests.json`, `append-transaction-unit-tail.log` | Staged complete-view reservation planner: targeted, ASan/UBSan and full AMD units pass. Production routing remains unchanged until coordinated completion is integrated. |
| `unit-tail.log` | Combined committed-source `make -j12 test`, exit 0 on AMD; full console at `/tmp/efs-readiness-final-unit.log`. |
| `w73-candidate-failure.log`, `w73-candidate-fuse.log` | Failed synchronous append integration; short write and subsequent NOT_PRIMARY after publication. |
| `append-boundary-*` | Repeated 128 KiB append/fsync fails EIO without the W73 candidate; trace confirms NOT_PRIMARY after publishing cross-group lanes. |
| `w73-not-accepted.patch` | Diagnostic candidate only, **not applied** to production sources. Fix W64 cross-group authority/routing before retrying. |

Raw server logs and private fixtures remain on AMD. Manifests identify binaries
by SHA-256; private copies exclude `.git`, so their embedded version strings are
not sufficient source identities. W64 code is `9ddcf7a2`; persistence code is
`e77e1f13`; bounded DNS is `011ae854`. Persistence live fixture predates DNS;
combined unit checks include it. Process restart is not physical power loss.
See [the task ledger](../../../docs/status/replay-readiness-20261008.md) for
remaining W64/W88, D27, W72/W86 and D25 obligations.
