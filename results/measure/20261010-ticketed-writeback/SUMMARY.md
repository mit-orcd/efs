# Summary — 20261010-ticketed-writeback

## Purpose and linked item

AMD retained-PUT / ticketed-publication evidence hour (started 02:44:12 UTC,
2026-10-10) on branch `devel-w66`: budgeted pre-PUT admission, ticketed
publication with unique PUT IDs, published-only retirement, and the final
focused verification. W86/W88 staged-integration work. Linked from
`docs/status/dependency-package-20261009.md` (sections around lines 899 and
937, "Published-only retirement and final checkpoint — Oct 10 UTC"), which
points at this run's README.md and directory. Added by commit `7ff7cb708`
(raw-evidence branch).

## Verdict

**FAIL** (mixed; one unresolved live failure).
- Owner portion (`22c8c1f9`, fixture `gc-direct-452dqomx`): **PASS** —
  exact/discarded admission replies, immutable body, ordered client membership,
  completed revocation, cold ticket replay; core 212/212, peer 64/64,
  append/sync/physical-GC/restart.
- Publication portion (`70daa5a4`, fixture `gc-direct-6rj80dwg`): **PASS** —
  same/cross-group pre-PUT admission, atomic mapping/PUBLISHED ticket, exact
  cache consumption, lost replies, lane leader death, cold retired mapping
  replay; core 212/212, peer 64/64.
- Retirement/window portion (fixture `gc-direct-029di86m`): **FAIL** — the 130
  extra upload/publication/retirement cycles per stream passed, but subsequent
  default POSIX CREATE returned BUSY persistently; suite cancelled, exit 143.
  No peer/final-GC claim. The CREATE BUSY cascade remains unresolved
  (EBUSY failures visible in `retirement/window-live.log` for
  concurrent_create_unlink_two_proc, concurrent_write_and_readdir,
  concurrent_appends_two_proc, ...).
- Final focused verification (`f626c2ba`, fixture `gc-direct-g2hs6cwl`):
  **PASS** — typed writeback same/cross, discarded publication/retirement
  replies, lane leader death, cold floor replay, late admission refusal,
  physical GC, offline-member/rejoin/restart fencing. Deliberately excluded
  the POSIX suites and does not close the CREATE BUSY failure.
- All complete code regression runs passed (after repairing an extracted lease
  harness boundary); Linux builds warning-free; ASan/UBSan ownership gates
  passed after the idle typed-entry reuse fix. First window attempt
  `gc-direct-e1wo8imb` failed at the next accepted typed write (extra-presence
  accounting blocked reuse after terminal body release) — fixed.

## Source commit and build identity

Commits `22c8c1f9`, `70daa5a4`, `f626c2ba` (stated in README and status page).
Per-file source SHA256 for changed files in
`retirement/local-source-sha256.txt` and `retirement/remote-source-sha256.txt`
(local/AMD match). No deployed-version file.

## Environment

One AMD host, four daemons, direct I/O, TCP transport; unique private stores
(`/data1/efs/evidence/lease-retention-fixtures/gc-direct-*`) and the two authorized
test mounts (`/data1/efs/mnt`, `/data1/efs/mnt2`); installed service restored
between cycles. No hardware power-loss or distributed hardware-RDMA claim; no
D25/default FUSE activation implied.

## Method

Live fixture harness cycles per portion (`owner/live.log`,
`publication/live.log`, `retirement/window-live.log`,
`retirement/focused-live.log`): four-node bring-up, POSIX core/peer suites,
typed writeback wire checks (`typed-writeback-*.txt`), cold replay, lane
leader death, physical GC, 130-cycle retirement stress exceeding the
128-ticket bound. Offline: complete `make test` regressions
(`owner/regressions.log`, `publication/regressions.log`,
`retirement/regressions.log`, `retirement/repeat-regressions.log`) and
ASan/UBSan (`retirement/memory-sanitizer.log`).

## Results

| Portion | Fixture | Result |
|---|---|---|
| Retained pre-PUT admission (`22c8c1f9`) | gc-direct-452dqomx | PASS, core 212/212, peer 64/64 |
| Ticketed publication, unique PUT IDs (`70daa5a4`) | gc-direct-6rj80dwg | PASS, core 212/212, peer 64/64 |
| Published-only retirement + window stress | gc-direct-029di86m | FAIL — CREATE BUSY, exit 143 |
| Final focused verification (`f626c2ba`) | gc-direct-g2hs6cwl | PASS (no POSIX suites) |
| Complete Linux regressions / ASan/UBSan | — | PASS, warning-free |

## Limitations and remaining gates

Uploads still use legacy PUT_CHUNK after admission. Missing production gates:
versioned receive/store ticket validation, durable session storage fences,
stable captured server membership, all-member abandoned-upload deletion ACKs,
default FUSE flush ownership, revoked unknown-intent reconciliation.
PUBLISHED-only retirement does not retire ADMITTED or RECLAIMING bodies. The
window run's CREATE BUSY cascade is unresolved; read-only debugger attachment
was unavailable under host policy. Passive replay-window counters and opt-in
EFS_OPID_TRACE unresolved CREATE/lease diagnostics were added for the next
reproduction. Do not infer ticket retirement caused the default-path failure
merely from batch order.

## Evidence index

- `README.md` — run narrative and verdicts — `08fc65b51c10a913`
- `retirement/window-live.log` — failed window fixture, CREATE BUSY + exit 143 — `9cd4a2593cd39c8d`
- `retirement/focused-live.log` — final focused pass `gc-direct-g2hs6cwl` — `c6581a687ef325fd`
- `retirement/regressions.log` — complete Linux regressions — `c7617c6372ae7018`
- `retirement/repeat-regressions.log` — repeated regressions after focused gate — `f85ab5464040f631`
- `retirement/memory-sanitizer.log` — ASan/UBSan ownership gates — `0b1169622a4ca5db`
- `retirement/local-source-sha256.txt` — per-file source identity (local) — `5a5e508c72bcfb83`
- `owner/live.log` — admission portion live gates — `5009b62ba47992cc`
- `owner/regressions.log` — owner portion regressions — `16c5bccfd97ca1a6`
- `publication/live.log` — publication portion live gates — `2e3f1c77a3f017dc`
- `publication/regressions.log` — publication portion regressions — `f747d00f3a5aecae`
