# SUMMARY — results/measure/20261008-efs-command-options

EFS command-options gate: `.efs` command interface option handling, with the
final fixture's two-mount disabled/enabled gates and loaded gates.

## Purpose and linked item

- `.efs` command-directory options (feature-flagged interface). Related runs:
  `20261008-efs-commands`, `20261008-efs-command-perf`,
  `20261008-efs-followups`.

## Verdict

**PASS**: final fixture 2-client suite 64/64 (`loaded-gates.log`:
"POSIX 2-client: 64/64 pass, 0 fail (12.8s)"); options gate script
`options-gate.py`, production check `production-check.py`, deploy script
`deploy.sh`, remote evidence under `remote/`.

## Source commit and build identity

Per `remote/` and fixture logs in this dir.

## Environment

Final fixture with two mounts (fuse1/fuse2), disabled/enabled legs
(`disabled.log`, `n1-off.log`, `n1-restart.log`, `n1.log`).

## Method

`options-gate.py` + `production-check.py` over the final fixture; `deploy.sh`
rollout; remote host evidence in `remote/`.

## Results

2-client suite 64/64; options gates pass (logs in final-fixture/).

## Limitations and remaining gates

Command interface is feature-gated (EFS_COMMANDS); default-off production
behavior covered by production-check.py.

## Evidence index

- `options-gate.py`, `production-check.py`, `deploy.sh` — scripts
- `final-fixture/loaded-gates.log` — 64/64 record (run `shasum -a 256 <file> |
  cut -c1-16` here)
- `final-fixture/disabled.log`, `n1-off.log`, `n1-restart.log`, `remote/`
