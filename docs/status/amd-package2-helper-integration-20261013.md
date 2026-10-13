# AMD package 2: integrated helper retry-window check

**ON HOLD — final Package 2 checkpoint.** Owned fixtures/controllers are
finished; publisher PIDs restored. No further qualification or deployment
started. Runtime is left enabled on the exact source/profile below; pending
full-scale gates remain open until work is resumed.

**Affected gates PASS on `f7e3c81e2d5bd4715b6c6dc86debb10b093c0d03`.**
AMD now runs this combined source. Global W91/W92/W109 and ORCD full-scale
acceptance remain OPEN. This source includes the
[AMD-qualified e77f6030 LOCAL-subtree fix](query-subtree-continuation-20261013.md)
and bounds transient helper retries to one eight-second monotonic window.

## Reviewed delta and scope

Relative to e77f6030, storage, metadata, server, network, include/dependency C
inputs are identical. Runtime changes are command.py and its generated source.h;
other additions are diagnostic scripts/tests/docs. Eight attempts and existing
C RPC deadlines remain unchanged. read_result stops after a transient error
when its retry window is exhausted, caps the following sleep to the remaining
window, and checks again after sleep. It preserves the original OSError.
It **cannot interrupt a blocking filesystem call** and does not impose an
eight-second end-to-end command bound.

The existing e77 full normal/fault native and six complete-library query
sanitizer results remain their own exact-source evidence. Those suites were
not repeated or relabelled as AMD f7 passes; only affected integrated helper
and mounted gates were rerun. Additional ORCD exact-f7 checks reported by the
other chat are not counted as independently executed AMD evidence.

## Profile, identity and gates

AMD-395, four localhost daemons 17432–17435/two FUSE mounts, account efs;
commands client/server on, policy version 0/mask 3, files/TCP/direct,
packing/containers/expiry/repair experiment off. Data quota 17 GiB/node;
query caps 16/32 GiB and applicable 30/60-second mounted budgets unchanged.
Normal drain/install/start retained authoritative test stores; no wipe,
profiling, competing controller or production helper-count change.

| Affected gate | Result |
| --- | --- |
| Linux command/helper unit tests | PASS 34/34, generated source parity included |
| Exact prior-helper negative control | PASS: e77 read_result substituted only in memory; the slow-refusal test fails with 8 opens versus required 1 |
| Mounted commands/options/pages | PASS, each exit 0; filtering, immutable FD, peer convergence and fresh permission checks included |
| One quiet 8-writer/2-reader helper/refresh load | PASS 8,940/8,940 exact 256-byte peer payloads; both readers span 7 generations; only expected BUSY/no-text allowed |
| Four freshly created subtree helper probes | PASS, first results take 2.36–2.42s, eventual exact file count and peer bytes match; **zero transient helper-open errors observed** |
| Read-only pause of validated publishers 2/3 | PASS restoration/payload check; one open succeeds after restoration in 14.378s, with no retry; not an eight-second end-to-end bound or a live failed-open proof |

The loaded raw RPC readers have 412/420 successes and 384/378 expected BUSY
refusals. Five real stats helpers run during writes. Final file count+bytes
converge in 1.504s; writer p50/p95/p99 are 11.14/18.93/62.16ms, reader p99
125.48/129.54ms. The load keeps its 14-second admission and existing 12/30-second
command/future deadlines; no tuning or performance comparison is claimed.

The pause check uses SIGSTOP only for the two validated publisher PIDs, a
12-second restore timer, signal/finally SIGCONT restoration, no writes while
paused, and the existing 30-second helper and 25-second fresh-result budgets.
All four daemons and two clients are running after restoration. The observed
single 14.378s open demonstrates why the Python window cannot preempt filesystem
work. Slow failed-open deadline behavior is covered by unit/negative control;
actual mutation-time BUSY/no-text refusals are separately covered by load.

All 863 frozen Makefile/src/include/deps/scripts/tests inputs match the deployed
source. Sorted JSON input manifest SHA256:
f473473031b265c444e74c01fc0825a9f700645e0984b6fb75eb21ed2e9366b8.
Build stamp 2026-10-13T20:11:43Z on amd-395, branch devel. SHA256:

- efsd: be9dc0e1eafb6b0923b42d1cbd9acbb7a008236c5b298820869bcc282bc66d26
- efs-fuse: edd8dc38b5ab52e7f0b4f510811ac5e798e6834fe9ed5079ae25ce74bbb2c3e2
- efs-mgmt: caa6fbaf7b1d95eb569b25e7793036d67c9552a451be2f43ffa20cbd840e1583

## Reproduction and external evidence

Export exact f7e3c81e to an isolated Linux source/build workspace; compile all
three binaries with explicit full EFS_GIT_ID, EFS_GIT_DESCRIBE=f7e3c81e and
EFS_GIT_BRANCH=devel. Run python3 tests/test_efs_commands.py. Deploy via ordinary
exclusive AMD devops stop, copy only those built binaries, then start with the
profile above. Follow the
[existing command/options/pages/load reproduction](amd-package2-transition-load-20261013.md#final-checked-sort-qualification--2026-10-13) with fresh output paths and one
new load fixture. Remove only the owned options fixture before its mkdir.
No legacy-transition or unrelated suite repetition is required for this
helper-only delta.

To reproduce the negative control, import the f7 tests/test_efs_commands.py
module, extract only read_result's AST function from
`git show e77f6030:src/client/commands/command.py`, execute that function into
the test module's commands.__dict__, and run only
Commands.test_retry_stops_after_one_slow_refusal. Require exactly one assertion
failure, no errors, with 8 != 1. Do not overwrite the working/deployed source.

Raw evidence is outside Git on AMD under /data1/efs/package2-f7-*; build,
mounted, commands/options/pages/load-1, helper-negative, live-helper and
paused-helper markers all exit 0. Preserve previous failed source/capacity
cases and their independent verdicts. SHA256:

- helper-unit.log: d0082c689e78d9c43869146c8741a5df82385f0761fc4688340b04eacfd7940d
- helper-negative.log: e4f07188222b5bcccc0009b0358ac1bbe3ebe01062c83b0bfdea38a236f71a2e
- commands.log: b22b33fb4824783c3fb21030bdaf78bd6562f0b18833bd3a1e25d479f0c6dbd3
- options.log: 0f8e8568c990bf504bc19853e0ac9c388eceb8e90144ab8601991d24499782ba
- pages.log: 6cd4a619499d41100fff03eeaef2b7dad57289eddbd329212da262ae93143d83
- load-1.log: 176ae9c73a14f48a051e6ab7c726a28c517530548bb77a2c756146694ec1c1a7
- paused-helper.json: 8d98c45e078615c7942f3a6b960d16fc66779287775ed1a41967beb1c6f45155

No raw artifacts committed or push. Large-catalogue, non-LOCAL/larger-directory
fallback and final integrated-source acceptance require their own gates;
ordinary-file EOF diagnostics are outside this helper change.
