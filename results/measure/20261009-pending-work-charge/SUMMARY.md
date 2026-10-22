# SUMMARY — results/measure/20261009-pending-work-charge

Pending command charge checkpoint: admission charging for pending work, AMD
private fixture with pressure enabled only there.

## Purpose and linked item

- Pending-work admission charging (staging pressure policy).

## Verdict

**PASS** (per README.md): full Linux regression exit 0 (before the final
observation-only adjustment); final build + Raft/store/host focused tests exit
0, no build warnings; private GC + core/peer POSIX + cold-restart physical
deletion gate exit 0. Installed `978caaf7` service restored, both command
masks disabled, pressure off.

## Source commit and build identity

Per README.md; installed service `978caaf7`.

## Environment

AMD private source, direct I/O/TCP, admission pressure in fixture only.

## Method

Regressions + focused tests + GC/POSIX/cold-restart gates per README.md.

## Results

All listed gates pass.

## Limitations and remaining gates

Fixture-scoped pressure; production unchanged.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
