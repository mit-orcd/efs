# SUMMARY — results/measure/20260930-044100-mtime-utimens-close

ecopy "verification metadata mismatch" root-caused: a utimens between the last
write and close is lost — mtime becomes the close time. Reproducer + gate.

## Purpose and linked item

- Follow-up of `20260930-040600-postfix-review`: 153 ecopy mismatch lines
  traced to a metadata-semantics bug (utimens→close ordering), not transport.

## Verdict

**FAIL** (bug demonstrated): `mtime_repro.py` reproduces on an idle cluster
from one client; `gate-after-fix.txt` records the post-fix check. The
mismatched mtime = close time overwrote the utimens stamp.

## Source commit and build identity

Not recorded in the run.

## Environment

Idle cluster, single client (fstor007), RDMA; same build family as the
postfix-review run.

## Method

`mis_ctx.py` / `mtime_repro.py` reproducer; `ecopy-strace-context.txt` for the
original syscall context; `gate-after-fix.txt` for the verification.

## Results

utimens between last write and close lost on the affected build; the
gate-after-fix file records the repair check (see file for verdict detail).

## Limitations and remaining gates

Fix landed after this run; the regression coverage lives in the posix suite
mtime tests (attr_utimens*).

## Evidence index

- `SUMMARY.txt` — analysis (run `shasum -a 256 SUMMARY.txt | cut -c1-16` here)
- `mtime_repro.py`, `mis_ctx.py` — reproducer
- `ecopy-strace-context.txt`, `gate-after-fix.txt`
