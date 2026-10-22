# SUMMARY — results/measure/20261009-w97-flow-enhanced

W97 flow-enhanced harness: capture/analyze/deploy/loader scripts for the
enhanced flow measurement — harness only, no run output in this dir.

## Purpose and linked item

- **W97** enhanced flow measurement harness (`analyze.py` histograms from
  cumulative atomic snapshot fields; `capture.py`, `deploy.sh`, `finish.sh`,
  `loader.py`, `run-gates.sh`, `run.sh`).

## Verdict

**NOTRUN** (harness only): the dir contains the scripts and the timebase
mechanism but no run output; the measured runs live in the sibling
`20261009-w97-flow-*` dirs.

## Source commit and build identity

Not recorded in the run.

## Environment

Harness for the W97 flow runs (NUC line).

## Method

Scripts: deploy.sh / loader.py / capture.py / analyze.py / finish.sh /
run-gates.sh / run.sh.

## Results

No measurement data in this dir — harness only.

## Limitations and remaining gates

See the sibling w97-flow runs for results.

## Evidence index

- `analyze.py`, `capture.py`, `deploy.sh`, `finish.sh`, `loader.py`,
  `run-gates.sh`, `run.sh` (run `shasum -a 256 <file> | cut -c1-16` here)
