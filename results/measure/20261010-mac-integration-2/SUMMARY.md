# Summary — 20261010-mac-integration-2

## Purpose and linked item

Mac (Tart VM) devel integration acceptance: merge every captured branch tip
into devel and re-run the full Mac correctness suites on each new immutable
snapshot. Added by commit `ab2bdb2d3` ("Record all-tip Mac deployment,
integration fixes and full passing acceptance"). No status page links this run
by slug; it continues the devel integration-gate pattern of
`docs/archive/status-20261011/devel-integration-20261009.md` (linked item:
devel combined branch gates; linkage to this run inferred from README/commit).

## Verdict

PASS — final captured all-branch snapshot `37ad6a8790c8` passed all Mac suites
with zero failures/skips/unrun. Two mid-run code corrections were required and
re-validated: `1a78d768` (retained CREATE unknown-OK → EBUSY mapped to BUSY)
and `d787ced245a5` (MKDIR 30 s bounded dual-group retry). Verdict applies only
to correctness under extended Mac budgets; original latency targets NOTRUN.

## Source commit and build identity

- Final accepted source: `37ad6a8790c8aaa2cf40318e38411d98084d0eae` on devel
  (`completion-batch/source-build.txt`).
- Earlier snapshots: initial `4be9ecf6c51c`, `1a78d768d2a0`, `03db5552cc2d`,
  `cc00aacf8375`, `d787ced245a5` (per-batch `source-build.txt` files).

## Environment

Mac host with Tart VMs (Fedora aarch64; kernels 6.14.0 / 6.19.14-108.fc42):
three storage nodes efs1/efs2/efs3, gateway efsgw (NFS re-export) and a seed
client; all daemons/clients run as Linux user `efs`. Fixture: 300 MiB
content/EOF cold verification. Transport not recorded (Mac profile is TCP;
no RDMA on this harness). Buffered vs direct I/O not recorded in the run.

## Method

Run-dir scripts `efs-mac-integration2-*.sh` and per-batch
`*/efs-mac-*-final*.sh` drive: freeze branch tips (`integration-refs.tsv`),
immutable source-archive build + full Linux unit suite, doc gate, deploy to all
servers/VMs, then suites with Mac correctness budgets (core/persistence 600 s
per case, peer 2400 s per step): native core 212, POSIX core 212, persistence
prepare/verify 26+26 with normal drain/remount, two-client peer 64. Cleanup:
normal drains, fresh mount content check, NFS export restored.

## Results

| Suite (final 37ad6a8790c8) | Result | Duration |
| --- | --- | --- |
| Native core | 212/212 | 14.8 s |
| POSIX core | 212/212 | 1789.7 s |
| Persistence prepare | 26/26 | 313.7 s |
| Persistence verify (after normal remount) | 26/26 | 131.9 s |
| Two-client peer | 64/64 | 1643.6 s |
| Cold 300 MiB content/EOF check | PASS | 4.481 s |

Interruptions recorded (not counted as passes): guest reboots of efs2/efsgw
during the 1a78d768 persistence run; node-3 SSH outage during the recovered
peer run; seed root-XFS corruption repaired offline before the final batch.
Earlier failed TSVs are preserved under `initial/`, `after-fix/`,
`recovered/`, `latest-batch/`.

## Limitations and remaining gates

- Extended deadlines establish correctness only; original latency budgets unmet.
- The distinct AMD ticket-window CREATE BUSY stress trigger remains open.
- The unexplained guest reboots / node-3 outage are not claimed fixed.
- Results are per-snapshot; branches landing after the final capture untested.

## Evidence index

- `README.md` (5fe7882db51c6cb0) — full run narrative and per-batch results
- `accepted-verdicts.tsv` (1df090914c3da6e5) — accepted suite verdicts
- `completion-batch/final-verdicts.tsv` (0c6b91264c53791e) — exit statuses, all 0
- `completion-batch/peer-final.tsv` (053acd225fada886) — final peer 64/64
- `completion-batch/source-build.txt` (304d14783dbe8019) — final source SHA
- `completion-batch/ancestry.tsv` (4266dbf32e0ab0c7) — branch-tip ancestry audit
- `integration-refs.tsv` (6f113f2bd452dd9a) — frozen branch tips
- `build-unit.log` (fd7b96f70625a2b1) — initial full Linux unit suite
- `build-unit-fix.log` (0b435eb9609fff17) — fixed-build unit suite
- `completion-batch/health-final.log` (6b6d840aca6e786a) — final 3/3 health
