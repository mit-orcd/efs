# F5 decoder check on devel 8e53122cd (Task CC)

**Purpose:** acceptance lane F5 of the [Stage 1 matrix](../stage1-acceptance-matrix-20261030.md) for **K2** (proposal RPC accepts tags this build cannot apply, W140; [definition](acceptance-definitions-20261031.md#k2)). **Verdict: PASS, 26 of 26 targets** (rc 0 each; no crash, sanitizer report, refusal-path failure or timeout). K2 stays VERIFY: VERIFIED is reserved for the frozen candidate, and F1 and the cells' `c5-protocol-live` are separate runs.

**Target count.** The definition says "25 of 25" (24 record targets plus apply, the [Task C](task-c-20261025.md) count on `eb936c71f`). `tests/test_decoder_inputs.py` `TARGETS` now holds 25 record targets (24 plus `declaration`), so the script runs 26 with `apply`. The 7,200 s declaration target is counted in the matrix F5 text but not in the "25" of the definition; the pass condition is rc 0 for every target the script runs.

## Environment

- Source: devel `8e53122cd6c9bdd56ac020f1c1be4a8a5e4445af`, shipped with `git archive HEAD` (clean tree, no local changes).
- Host: amd (32 CPU, Ubuntu, ext4 home), idle: load 0.56 before, load average 3.6 at the end (`uptime` after: 00:21:50, load 3.64 3.55 3.99).
- Compiler: Ubuntu clang 21.1.8 as `clang-21` (no unversioned `clang` on amd, hence `CC=clang-21`); Python 3.14.4. Flags from the script: `-O1 -g -std=c99 -D_GNU_SOURCE -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all`; `ASAN_OPTIONS=abort_on_error=1:detect_leaks=1`, `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Fixed fuzz seed `0x20261025c`.
- No mount, cluster, port or FUSE: the binaries are `tests/decoders/check` and `tests/decoders/apply`. Default durations: 1,200 s per record target, 7,200 s for `declaration` and `apply`.

## Reproduce

```sh
git archive 8e53122cd | ssh amd "mkdir -p ~/efs-taskCC/src ~/efs-taskCC/tmp && tar -x -C ~/efs-taskCC/src"
ssh amd 'cd ~/efs-taskCC/src && TMPDIR=$HOME/efs-taskCC/tmp CC=clang-21 \
  python3 scripts/decoder_check.py --jobs 8 --out $HOME/efs-artifacts/taskCC-20261031/decoder'
```

Exit status 0 means all targets passed. A failure keeps its exact input under `<out>/<target>/` (`current.bin`) and its log in `run.log`. Cleanup: `rm -rf ~/efs-taskCC/src ~/efs-taskCC/tmp`. `make decoder-check` runs the script with its defaults (2 jobs, gcc), not this command.

## Results

| target | result | wall (s) | mutations (apply: cases) |
|---|---|---|---|
| `apply` | PASS | 7200 | 13,202,822,556 |
| `audit-reply` | PASS | 1200 | 38,487,320 |
| `audit-request` | PASS | 1200 | 38,643,796 |
| `boot` | PASS | 1200 | 38,500,727 |
| `cleanup-probe` | PASS | 1200 | 38,351,650 |
| `declaration` | PASS | 7200 | 251,594,144 |
| `degraded` | PASS | 1200 | 38,019,913 |
| `degraded-command` | PASS | 1200 | 38,201,260 |
| `discovery-answer` | PASS | 1200 | 29,339,948 |
| `discovery-probe` | PASS | 1200 | 29,585,713 |
| `fragment` | PASS | 1200 | 29,170,858 |
| `fragment-verify` | PASS | 1200 | 29,372,065 |
| `hello` | PASS | 1200 | 26,969,547 |
| `identity` | PASS | 1200 | 29,607,910 |
| `member-view` | PASS | 1200 | 28,443,011 |
| `membership` | PASS | 1200 | 28,266,713 |
| `pack-degraded` | PASS | 1200 | 28,614,059 |
| `put-header` | PASS | 1200 | 28,450,674 |
| `receipt` | PASS | 1200 | 28,501,596 |
| `recovery-command` | PASS | 1200 | 28,235,793 |
| `recovery-reply` | PASS | 1200 | 27,789,250 |
| `recovery-request` | PASS | 1200 | 27,772,303 |
| `ticket` | PASS | 1200 | 27,530,023 |
| `ticket-batch` | PASS | 1200 | 27,525,765 |
| `ticket-drain` | PASS | 1200 | 27,660,441 |
| `ticket-settle` | PASS | 1200 | 27,580,554 |

- **Passed 26 of 26.** Run wall time 7,215 s (120 min, `date` before and after), set by the two 7,200 s targets that started first; the 24 other targets ran six at a time beside them and finished well inside that window.
- **Against the estimate:** 135 min estimated, 120 min measured, 11 percent lower, inside the 20 percent band, so the matrix F5 row and totals are unchanged.
- Refusal-path lines in the logs (`handler: refused declaration of protocol N ... closing`, `raft-host: recovery-mark ... invalid argument`) are the expected refusals the targets provoke, not failures.

## Artifacts

Raw output stays outside Git on amd: `/home/maik/efs-artifacts/taskCC-20261031/` (`decoder/<target>/run.log`, `decoder/summary.json`, `f5.log`, uptime and timestamps). The 78 MB sanitized build tree was removed (`PRUNED.txt`; recreate from the commit with the command above). `SHA256SUMS` in that directory has sha256 `574d66d4ab6d4c10adff9bb9c5542331cb4fa1fb5f26a3f43da1767e4f6d23d2`. The script's own `decoder/SHA256SUMS` is also kept.

## Limitations

Loopback and sanitizer only (decision BT-1: no distinct-host or RDMA decoder run). One seed, one host. Not a frozen-candidate run: re-run F5 on the candidate for VERIFIED.
