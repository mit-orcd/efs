# SUMMARY — results/measure/20261009-tiny-five-minutes

NUC tiny-file five-minute profile: the startup burst falls to a lower sustained
rate; strongest evidence is NVMe durability latency shared by n3/n4.

## Purpose and linked item

- Tiny-file workload sustained-rate investigation (follows
  `20261008-nuc-tiny-create`).

## Verdict

**PASS** (capture completed; finding): the five-minute capture shows no
monotonic decline after the startup burst; the displayed rate is cumulative
created/elapsed rather than an interval rate; the strongest evidence is
durability latency on the NVMe shared by n3/n4 — not Python or client CPU
saturation.

## Source commit and build identity

Not recorded in the run.

## Environment

NUC production, tiny-file loader, five-minute capture window.

## Method

Five-minute profile of the running loader (README.md).

## Results

Sustained-rate finding per the verdict (NVMe durability latency is the limiter).

## Limitations and remaining gates

Cumulative-rate display caveat; interval rates need the per-window analysis.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  captures in this dir
