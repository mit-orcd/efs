# Summary — 20261008-replay-readiness

## Purpose and linked item

AMD replay, persistence and production-readiness checkpoint for the Oct 8, 2026
replay/writeback package: W64 durable append reservation replay, W73 synchronous
append publication, D27 bounded DNS/partial-body drain, and W71-style persistence
fencing. Linked ledger: `docs/status/replay-readiness-20261008.md`. Run added by
commits including `3798deb3b`, `5ff9ad5af`, `593becf31`.

## Verdict

PASS for the checkpoint as a partial package — explicitly **not** production
activation approval. All post-fix reruns pass (replay/, persistence/,
append-authority-pass, shutdown-recovery-pass, stop-flight-pass,
stop-partial-pass, sync-publication-pass, streaming-append-pass,
append-budget-pass, sync-fault-pass) and the combined AMD unit suite exits 0.
Retained historical FAILURES, not upgraded: the rejected W73 candidate
(short write then NOT_PRIMARY after publication), 128 KiB append/fsync EIO
boundary failures without the fix, and the pre-fix shutdown partial-body drain
failure (`shutdown-before-fix/`).

## Source commit and build identity

W64 code `9ddcf7a2`; persistence code `e77e1f13`; bounded DNS `011ae854`
(recorded in README). Binaries identified by SHA-256 in per-phase
`manifest.json`; private source copies exclude `.git`, so embedded version
strings are not sufficient source identities.

## Environment

One AMD host, four daemons, private fixtures; functional runs use direct I/O
over TCP. Persistence live fixture predates the DNS change; combined unit checks
include it. No efs-bench changes.

## Method

Per-phase harness runs (scripts under `tests/live/`, e.g. the GC/reclamation
driver that emits `manifest.json`/`final-status.json`) covering lost committed
replies, 32 concurrent identical requests, explicit group-0 leader death, ACK
retirement, changed-payload refusal, cold replay with exact byte audits, GC and
restart. Units: `make -j12 test` on AMD (exit 0; `unit-tail.log`). Append
transaction planner: targeted, ASan/UBSan and full AMD units
(`append-transaction-tests.json`). Fixtures and raw server logs remain on AMD.

## Results

| Gate | Outcome |
| --- | --- |
| replay/ broad gates | PASS; POSIX 216 pass / 0 fail / 1 skip; peer 64/64; exact 800-record cold audit; physical GC/restart |
| persistence/ broad gates | PASS, plus active shutdown; store buffered/direct/bounce fault tests pass |
| append transaction planner units | PASS (targeted, ASan/UBSan, full AMD units) |
| Combined `make -j12 test` | exit 0 |
| W73 candidate (historical) | FAIL — short write, NOT_PRIMARY after cross-group publication |
| Append boundary without fix (historical) | FAIL — repeated 128 KiB append/fsync EIO |

## Limitations and remaining gates

Process restart is not physical power loss (W71 hardware acceptance owed).
W88 production REPORT identity/receipt authority, W72/W86 sessions/fencing and
D25 activation remain open per the ledger. The persistence live fixture predates
the bounded-DNS change. Raw daemon logs and private stores are not committed
(remain on AMD).

## Evidence index

- `README.md` (06773ee9541d5c76) — package scope, pass/fail ledger, source identities
- `replay/final-status.json` (1fce9200e5aeb5af) — clean post-replay GC/replay status, zero errors
- `replay/manifest.json` (a2252781954ec774) — fixture and binary hashes for the replay phase
- `replay/posix.tsv` (62a0dbba625c52f2) — POSIX suite verdicts for the replay phase
- `persistence/final-status.json` (9ffde2cf795b76b6) — persistence phase final status
- `persistence/manifest.json` (559eb4317a8bf34e) — persistence fixture/binary manifest
- `append-authority-pass/final-status.json` (2b3881d461d579a6) — post-fix append authority rerun status
- `append-budget-pass/final-status.json` (5f5dead61aa1b3ff) — append budget gate status
- `sync-fault-pass/final-status.json` (37987001c571cfa4) — O_SYNC/O_DSYNC fault recovery status
- `unit-tail.log` (68ad6eaaddca8387) — combined committed-source unit suite tail, exit 0
- `w73-candidate-failure.log` (0f9626b97ea71550) — retained rejected-candidate failure
- `append-boundary-failure.log` (6d150aaec81a4c61) — retained pre-fix EIO boundary failure
- `shutdown-before-fix/console.log` (1ba26a2f1dfae578) — retained pre-fix partial-body drain failure
