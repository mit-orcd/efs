# 20261011-orcd-query-ab — evidence summary

## Purpose and linked item

A/B diagnostic for W109 (loaded query/virtual-command contention on ORCD):
measure the running tiny-files benchmark's throughput with derived `.efs`
queries enabled versus temporarily disabled via commands policy, to test
whether query refresh contributes to the loaded slowdown. Linked status page:
`docs/status/orcd-communication-20261011.md` (cites this run; W109 remains
open). Run preserved by commit `5a5caee9c`. An existing `README.md` in the run
dir records the method and numbers.

## Verdict

**PASS** as a diagnostic (completed cleanly; policy restored and verified).
The finding is suggestive, not a gate: mean disabled rate was ~8.13% higher
than enabled, supporting query refresh as one contention contributor. It is
not a repeated matched performance acceptance, not latency attribution, and
does not close W109.

## Source commit and build identity

- Runtime `52bc8edb` (unchanged from the surrounding ORCD rollout), per
  README. No binary manifest in the run.
- The benchmark process was the existing one (fcstor007 PID 1381139, screen
  `1334126.tiny-files`); not restarted, so no new build identity applies.

## Environment

- ORCD cluster (fcstor003–006 servers, fcstor007–013 clients), runtime
  `52bc8edb`, RDMA `mlx5_2`, direct-I/O servers — inherited from the
  20261011 rollout context.
- Workload: existing 16-worker tiny-files benchmark on fcstor007 targeting
  10,000,000 files; ~1.42–1.48 M files completed during the diagnostic window
  (05:31–05:34 UTC, 2026-10-11).
- Diagnostic toggled only commands policy (derived `.efs` queries); file-data
  durability, transport mode and metadata routing unchanged.

## Method

Bounded sequence (recorded in `result.json` and README): verify the same
benchmark PID and a monotonic file count from screen hardcopies; sample
30 s windows — enabled-before, disabled-1, disabled-2, enabled-after — with
15 s settling intervals before the first disabled and final enabled windows.
Policy changed from version 0/mask 3 to version 1/mask 0, then restored to
mask 3 at version 2 in a `finally` block; a final read verified restoration.
Counts come from the last progress line of screen hardcopies; progress
batching introduces boundary quantization. Remote hardcopies remain at
`/tmp/efs-w109-query-ab-mulcpajb/` (outside Git).

## Results

| Phase | Window | Files/s |
| --- | --- | --- |
| Enabled before | 30 s | 409.51 |
| Disabled, first | 30 s | 438.79 |
| Disabled, second | 30 s | 442.86 |
| Enabled after | 30 s | 405.84 |

Mean disabled 440.83 files/s vs mean enabled 407.67 files/s: ~8.13% higher
with queries paused. Policy events (get/set rc=0) and `restored: [2, 3]` are
recorded in `result.json`; `errors` is empty. No competing policy change was
observed.

## Limitations and remaining gates

- Single bounded sequence; not repeated, not a matched acceptance test.
- ~8% delta leaves most of the loaded overhead unexplained: live CREATE/apply
  waits and legacy fragment padding still need investigation and repair.
- W109 remains open.
- Screen-hardcopy sampling has boundary quantization; raw hardcopies are not
  in Git.

## Evidence index

| File | sha256 (16) | Content |
| --- | --- | --- |
| `result.json` | 3581cb7fb144492a | Timestamps, exact counts, monotonic intervals, policy replies, restoration evidence |
| `capture.txt` | e0c053f14909b3db | Full diagnostic stdout (per-window records, RESULT_JSON line) |
| `README.md` | 14eb0740b6875473 | Pre-existing method/result write-up |
