# SUMMARY — results/measure/20261009-all-efsd

Simultaneous CPU profile of all four production NUC efsd PIDs under the active
tiny-file workload.

## Purpose and linked item

- Production CPU attribution for efsd under the eight-worker tiny-file
  workload (output tiny-ordinal-093295aa, target 20,000).

## Verdict

**PASS** (capture completed): 30 s perf at 199 Hz across the four production
PIDs, 6,814 samples, zero lost; /proc counters bracket 31.951 s including perf
startup/teardown.

## Source commit and build identity

Installed build `093295be288b`, direct I/O/TCP, `.efs` enabled.

## Environment

NUC production service, four efsd PIDs; no workload/code/deployment changes for
the capture.

## Method

perf at 199 Hz per PID + /proc counter brackets; reports use relative
percentages per filtered PID (inclusive rows overlap — do not sum).

## Results

Per-node CPU shares and main inclusive sampled paths are tabulated in
README.md (per-node PID, CPU %, top paths).

## Limitations and remaining gates

Relative per-PID percentages; inclusive rows overlap and must not be summed.

## Evidence index

- `README.md` — narrative + table (run `shasum -a 256 README.md | cut -c1-16`
  here); perf reports in this dir
