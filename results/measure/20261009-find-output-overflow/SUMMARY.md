# SUMMARY — results/measure/20261009-find-output-overflow

Find output overflow probe: bounded output with status -22 (INVAL) when the
term exceeds limits; snapshot/header behavior recorded.

## Purpose and linked item

- `.efs` find output-limit handling: prove overflow is refused cleanly, not
  silently truncated.

## Verdict

**PASS** (probe completed): find-overflow-probe.json shows term ".txt",
match=3, max_output=262144, **status=-22** (EINVAL), checked_dirs=17,
elapsed 33 ms — overflow refused with a status, and the root-stats header
("Results as of 2 seconds ago; complete for this snapshot. Background refresh
may lag recent writes. Snapshot 17542146024255906:438; no refresh error") is
truthful.

## Source commit and build identity

Snapshot boot 17542146024255906, generation 461 (probe records).

## Environment

NUC deployment, find command over the mounted tree.

## Method

`find-overflow-probe.py` probes (find-overflow-probe.json,
find-overflow-scoped.json + .err), root stats capture
(efs-find-root-stats.json/.err), investigation notes
(efs-find-txt-investigation.err).

## Results

Overflow → status -22 with bounded output; headers carry snapshot + freshness
truth.

## Limitations and remaining gates

Probe scope; suite coverage in the command-pages run.

## Evidence index

- `find-overflow-probe.json`, `find-overflow-scoped.json`,
  `efs-find-root-stats.json`, `find-overflow-probe.py`,
  `efs-find-txt-investigation.err` (run `shasum -a 256 <file> | cut -c1-16`
  here)
