# Task P — second Stage 2 rehearsal (October 27–28)

`agent/devel-task-p` (tested `dbebab983`): TOTAL 516 FAIL 0 NOTRUN 14 (739s); prior 516/0/14 (887s). Evidence `docs/status/stage2-rehearsal-2-20261027.md`; intel2 `~/efs-artifacts/taskP-20261027*`. NOTRUN RDMA/physical domains: one-host TCP. No product defect; three driver fixes. Task P COMPLETE; release acceptance OPEN.

## Verdict and frozen sources

**Task P COMPLETE as a scoped rehearsal with three corrected driver findings.**
Both full matrices, the targeted D5 follow-up and final plan-fixed code gate
finished. This is not a new full-matrix PASS for the final driver: the
three-member historical FAIL and targeted correction are reported separately.
Task P is a scoped rehearsal, not alpha release acceptance. All 24 selected
gates plus the end-identity check ran on each topology. The original
three-member expiry failure remains FAIL; it is not relabelled after a fix.
The five-member corrected full run has no FAIL. Physical failure domains
remain NOTRUN because every member shares intel2.

The user authorized any available host rather than waiting for nuc.
Intel2 was idle with no private EFS process, mount or detached test job.
Task H/I/N data and running real clusters were left alone. Nothing was
pushed or merged into devel by Task P. Product fixes are outside this task's scope; no
product defect requiring a new K row was observed.

| Source | Base and role | Exact build identity |
| --- | --- | --- |
| `3ca4f7e6b233d381c1236ca5752d7f011cc40cec` | devel `24dad8e4b`, original two full matrices and code gate | `efsd 3ca4f7e6b233d381c1236ca5752d7f011cc40cec (branch agent/devel-task-p, built 2026-10-28T02:16:24Z on intel2)` |
| `b8f232f4023692a3d5161d12ed7e979bf66b29a9` | devel `e7e72e267` merged at `194c7f3d2`; D-P1 isolation fix; two full matrices and code gate | `efsd b8f232f4023692a3d5161d12ed7e979bf66b29a9 (branch agent/devel-task-p, built 2026-10-28T04:49:57Z on intel2)` |
| `dac2361ba4b70649dcf6207666eb7bc3ecb5333a` | same product as `b8f232f40`; D-P2 port fix; targeted D5 and code gate | `efsd dac2361ba4b70649dcf6207666eb7bc3ecb5333a (branch agent/devel-task-p, built 2026-10-28T12:49:51Z on intel2)` |
| `dbebab98382b5111d6be0b372782857c7d77630e` | same product and recipe; D-P3 plan fix; final code gate | `efsd dbebab98382b5111d6be0b372782857c7d77630e (branch agent/devel-task-p, built 2026-10-28T13:10:14Z on intel2)` |

Product sources and Makefile are unchanged from `b8f232f40` through
`dbebab983`; only the driver and its regression differ. Later devel commits
are not qualified by these frozen runs. The task does not chase a moving
HEAD or claim final-release qualification for another candidate.

## Environment and reproduction

Intel2: Intel Core Ultra 7 256V, eight CPUs, approximately 15 GiB RAM,
FUSE 3.16.2, private home/XFS storage with 838 GiB available at preflight.
Eight build/test jobs. TCP loopback, O_DIRECT fragment I/O, 2+1 profile,
packing and `.efs` enabled, containers off. Main fixtures use 4 GiB/member,
three or five daemons and two FUSE mounts. E4 uses 2,000 small files/member
and two 32 MiB files/member. Private helper gates may use their own member
counts: D5 and GC use four; A13 grows three to five; soak grows five to seven.
Thus passing a helper is not proof it used the parent topology.

The loopback matrix now explicitly selects the existing **900-second
rehearsal** soak profile. Other topology defaults retain four-hour endurance.
This workload choice introduces no product flag. Rehearsal RSS growth is
explicitly NOTRUN: the interrupted short windows cannot qualify endurance.

Committed serial controller `scripts/task_p_rehearsal.py` records commands,
process walls and exit codes; runs each matrix to its end despite gate
failures; verifies its manifest; then runs `make test-all` with eight jobs.
It refuses to advance with owned processes/mounts still active. The second
freeze waited for the original controller, never altered an active tree and
never overlapped measurements. Use a fresh output/source root per freeze:

```sh
# Exact Git archive exported to ~/efs-taskP-UNIQUE/src on intel2.
export EFS_GIT_ID=FULL_COMMIT EFS_GIT_DESCRIBE=FULL_COMMIT
export EFS_GIT_BRANCH=agent/devel-task-p EFS_FUSE_PREFIX=/usr
export EFS_BUILD_JOBS=8 EFS_TEST_JOBS=8 EFS_TEST_TARGET=test-all
SCREENDIR="$HOME/.screen" screen -dmS taskP-UNIQUE bash -c \
 'cd "$HOME/efs-taskP-UNIQUE/src" && python3 scripts/task_p_rehearsal.py FULL_COMMIT "$HOME/efs-artifacts/taskP-20261027-UNIQUE"'
```

The controller executes plan validation, the Linux driver, standalone and
soak regressions, both matrix `run` commands with `--allow all`,
`qualify.py verify-sums` and `scripts/make_test_summary.sh`. D-P2 targeted
command is the same three-member matrix with `--gates identity,d5-expiry`,
then a separate eight-job code gate. The exact detached shell controller,
commands and exit receipts are retained in the D5 evidence root.
Mac driver testing reached the Linux-only `/proc` identity step and could
not complete; both later Linux runs passed. The Mac failure is not counted
as a Linux qualification pass.

## Results and classifications

| Frozen source / stage | PASS / FAIL / NOTRUN | Gate-sequence / outer wall (s) |
| --- | --- | --- |
| Original `3ca4f7e6b`, loopback-3 | 22 / 1 / 2 | 3615.588 / 3624.467 |
| Original `3ca4f7e6b`, loopback-5 | 23 / 0 / 2 | 4775.805 / 4793.591 |
| Corrected `b8f232f40`, loopback-3 | 23 / 1 / 1 | 4859.454 / 4869.844 |
| Corrected `b8f232f40`, loopback-5 | 24 / 0 / 1 | 5494.414 / 5507.773 |
| `dac2361ba`, targeted three-member identity/D5/end-identity | 3 / 0 / 0 | 283.707 / 290.23 |

No untouched same-host devel A/B is claimed: the original source already
contains the rehearsal-selection correction. The original code gate: **TOTAL 514 FAIL 0 NOTRUN 14 STATUS INCOMPLETE
(740s), OVERBUDGET 2, BUILD 0s**. Corrected full-matrix freeze:
**TOTAL 516 FAIL 0 NOTRUN 14 STATUS INCOMPLETE (858s), OVERBUDGET 4,
BUILD 0s**. Both durations include command scheduling, not their earlier
29.752/29.744-second initial builds. They are different source freezes;
this is not a controlled throughput A/B. D-P2 code gate: **TOTAL 516 FAIL 0 NOTRUN 14 STATUS INCOMPLETE (887s),
OVERBUDGET 2, BUILD 0s**. D-P3 final driver-only code gate: **TOTAL 516 FAIL 0 NOTRUN 14 STATUS
INCOMPLETE (739s), OVERBUDGET 4, BUILD 0s**. W160 target remains 1100 s;
no budgets, slots or timeouts changed.

Every non-PASS is classified:

- **D-P1, driver:** original soak NOTRUN on both topologies because its
  quiet-host guard detected the driver's parent daemons. Fixed at `b8f232f40`:
  suspend only the owned fixture with restart permission, check mount users,
  require clean stops, then restore readiness and mounts. The quiet-host guard
  is unchanged. External-cluster ownership, missing permission, stop/restore
  failures, original plugin failures and exceptions have negative controls.
  The new regression fails against `194c7f3d2`; fixed driver passes. Both
  corrected full runs' soaks PASS (1151.845/1105.593 s), all nine events:
  five clean member stops, one SIGKILL, client restart, additions six/seven.
  Each checks 512 current file slots and acknowledged appends on both mounts.
- **D-P2, driver:** original and corrected three-member D5 FAIL before
  workload (`Address already in use`). Main endpoints 18940–18942 overlapped
  D5's requested 18940–18943. Five-member D5 used 19640–19643 and passed.
  `dac2361ba` advances the four-port D5 range past advertised parent endpoints
  and reserved ports, rejecting overflow. Regression covers three/five/sixteen
  members and caller overlap; unchanged old driver fails exactly the
  three-member collision assertion. Corrected D5 PASS in 282.985 s on ports 18944–18947, including 218/218 POSIX cases with expiry enabled.
- **D-P3, driver (introduced by D-P1):** standalone `--plan` entered the
  in-process loopback stop/start path, bypassing Runner's command dry-run
  guard. The driver regression grew from 4.015 to 187.334 s. Fixed at
  `dbebab983`: plan mode records only the plugin, with no lifecycle/readiness
  effects. A mock-state negative control requires unchanged running/mounted
  state and only one plugin call; it fails against `dac2361ba`. Final Linux
  driver PASS in 4.02 s (from 187.334 s); the eight-job code gate PASSes
  every command that ran (516 commands, 14 scope markers) in 739 s. It ran
  after D-P2's closed gate, in a fresh source tree.
- **Environment/scope:** `topology` NOTRUN in all complete runs: one physical
  host is not three/five failure domains. POSIX extras pass four cases but
  skip `mmap_write_read` with ENODEV; mmap coverage is not claimed. Soak's
  RSS sub-verdict is NOTRUN by rehearsal design, not a passing memory gate.
- **Code-gate limits:** 14 NOTRUN markers comprise absent external cluster
  scripts; absent `valgrind/memcheck.h`; nine RDMA paths without usable IB;
  the fault-build-only follower-bound subcase; and two deliberate missing-tool
  selftest markers. The last two do not establish real tool absence.
  The marked RDMA/fault subcases do not make their entire enclosing command
  untested. None is converted to a product pass.

Corrected full-run POSIX core passes 218/218 on each parent topology;
two-client passes 68/68 each. D4 reports zero workload errors and zero bad
payloads; 129,871/124,615 surviving load files, drain 113.1/103.3 s under the
900 s limit. E4 verifies 6,000 small + six 32 MiB files, then 10,000 + ten,
from cold readers and verifies the retained subset after deletion/reclaim.
D1 verifies overwritten, truncated, unlinked and retained packed content
before/after packing, compaction and cold restart. These are fixture-specific
payload checks, not a verification of an arbitrary retained cluster.

## Per-gate wall comparison with Task A

`scripts/task_p_compare.py ARTIFACT_ROOT` reads Task A's corrected full-run
table and these closed result files. Every wall appears below. Task A ran
on AMD with overlapping matrices/other work; Task P is serial on intel2.
D5 and soak have no Task A corrected-full baseline. D5's failed 0.723 s is
setup refusal, not an expiry measurement. GC's Task A baseline was FAIL.

| Gate | Task A 3 / 5 (s) | Task P 3 / 5 (s) | Doubled vs Task A |
| --- | --- | --- | --- |
| _identity-end | 0.253 / 0.305 | 0.721 / 0.717 | 3, 5 |
| a13-node-addition | 56.708 / 58.557 | 48.636 / 28.401 | — |
| append-failover | 484.524 / 359.030 | 742.203 / 701.914 | — |
| b-checks | 67.013 / 72.759 | 67.300 / 73.997 | — |
| b6-member-down | 81.204 / 79.947 | 78.014 / 83.003 | — |
| c5-protocol-live | 3.057 / 2.565 | 4.372 / 4.049 | — |
| d1-packing | 202.646 / 176.469 | 227.383 / 181.803 | — |
| d3-efs-load | 899.292 / 624.489 | 601.968 / 523.350 | — |
| d4-progress | 1079.868 / 1090.926 | 1189.930 / 1193.161 | — |
| d5-expiry | — / — | 0.723 / 185.623 | — |
| e1-manifest | 29.073 / 27.100 | 26.515 / 69.831 | 5 |
| e2-client-stop | 8.497 / 9.575 | 11.641 / 13.124 | — |
| e3-turnover | 63.599 / 62.964 | 84.384 / 74.830 | — |
| e4-scale | 335.949 / 430.095 | 92.898 / 580.707 | — |
| fragment-integrity | 10.629 / 8.724 | 10.846 / 10.773 | — |
| gc-reclamation | 280.261 / 281.698 | 204.772 / 183.569 | — |
| identity | 0.249 / 0.254 | 0.720 / 0.851 | 3, 5 |
| k13-tickets | 41.962 / 41.690 | 38.270 / 45.578 | — |
| k14-torn-read | 172.907 / 168.834 | 172.263 / 171.102 | — |
| posix-2client | 39.013 / 35.066 | 23.785 / 99.299 | 5 |
| posix-core | 28.602 / 35.424 | 29.745 / 60.803 | — |
| posix-durability | 29.909 / 28.170 | 17.578 / 40.987 | — |
| posix-extras | 30.551 / 22.950 | 32.200 / 60.326 | 5 |
| soak | — / — | 1151.845 / 1105.593 | — |
| topology | 0.293 / 0.249 | 0.457 / 0.723 | 5 |

**Timing findings:** T-P1: five-member E1 is 2.577×; T-P2: five-member
two-client POSIX is 2.832×; T-P3: five-member extras is 2.629× Task A's walls.
T-P4: identity, end-identity and five-member topology also double, but remain
sub-second. All are recorded findings;
no cause or product regression is proven across these different hosts/loads.
Append failover is slower (1.532×/1.955×) but below the doubling threshold.
No assertion, deadline, budget or slot was relaxed. The `b8f232f40` code gate's
four overbudget rows are K33 growth 138.2/135 s, K33 payload 55.5/45 s,
K22 admission 39.9/30 s and KV LSM 10.2/10 s. Final `dbebab983`
overbudget rows are K38 pressure 43.8/35 s, stop-progress 59.5/50 s, query
delta 10.2/10 s and K22 admission 42.8/30 s. These remain timing findings,
not hidden by a budget increase.

## F3 rehearsal ledger

The two full-run ledgers are retained verbatim in `loopback-N/f3-row.md`.
Their literal status is preserved below; a targeted correction does not
rewrite a historical full run. These are **loopback members, one physical
host**, not the distinct-host acceptance cells in the alpha checklist.

| Evidence | Three members, `b8f232f40` full run | Five members, `b8f232f40` full run |
| --- | --- | --- |
| Candidate and profile | NOTRUN (identity/end PASS; physical topology NOTRUN) | NOTRUN (identity/end PASS; physical topology NOTRUN) |
| Placement/capacity and decode | PASS (B checks, integrity, B6, A13) | PASS (same gates) |
| POSIX and concurrent I/O | PASS, with mmap skip above | PASS, with mmap skip above |
| Packing/shared-fragment GC | **FAIL D-P2** (GC/D1 PASS; D5 port collision); targeted D5 correction PASS at `dac2361ba` | PASS (GC/D1/D5) |
| Enabled `.efs` under load | PASS | PASS |
| Full payloads + orderly restart | PASS | PASS |
| Scale/progress evidence (E4, D4) | PASS (D4, rehearsal soak, E4); endurance RSS NOTRUN | PASS (same gates); endurance RSS NOTRUN |

## Retention and remaining scope

All raw evidence remains outside Git on intel2:

| Stage | Absolute path | Closed manifest |
| --- | --- | --- |
| Original two matrices/code gate | `/home/maik/efs-artifacts/taskP-20261027/` | 1,146 files; `6bc3d8b1ece4448ab6a9ca4b2736e239d083611574516588c8ba0ce692b58c2f` |
| D-P1 old/fixed regression | `/home/maik/efs-artifacts/taskP-20261027-driver-fix/` | 10 files; `7936d650b15987a197bd46f3ec34818d56011d6ab1b532bfc9cad93490ed30ce` |
| Corrected full matrices/code gate | `/home/maik/efs-artifacts/taskP-20261027-current-b8f232f40/` | 1,247 files; `19680855d3dddd287a69bd18b5e40c90594de1305e71a10a6a0edd80566d2739` |
| D-P2 negative/live/code gate | `/home/maik/efs-artifacts/taskP-20261027-d5-dac2361ba/` | 86 files; `199110e4ad9876c8b0bc130388f24f6cc95ac8f826cfe4ea8cad9dcafbfca734` |
| D-P3 negative/final gate | `/home/maik/efs-artifacts/taskP-20261027-plan-dbebab983/` | 30 files; `46377a3dd632e6af80efe1fef53f6c9b03ceddf5852d69fa15ec7d8a9b985d2d` |

Source archives: original SHA256
`d4c6b0ba97fa56c4a125253f90a265fcf2f9cb795364f892e75b82fc9938d912`;
corrected full-run SHA256
`6c220c88df8e723da40a2abf7cea2e7cd9aeffac43c34fda09ef1d0cf40e98fd`.
Both completed source trees pass their tracked-file hash manifests; the
corrected tree has 2,249 archived source files. All five driver manifests and the first three source manifests
passed before cleanup. Their inactive source/build/store trees, regression staging
and source archives have been pruned with `edelete`/PRUNED.txt, preserving
text methods/results and closed hashes. D-P3's source manifest and final raw manifest also verify. Its source/build,
payload/store tree and archives were pruned after the zero-failure gate.
Final archive SHA256:
`6493fda96ff1216a5be91dafc092d625427a07d12f8efbfcfc8101fbb4a140be`.
Both cleanup guards refused any failed/unexpected gate scope, checked mounts
and owned processes, and ran only after measurements closed. Final independent
inspection found no Task P trees, processes, mounts or detached screens.
The Mac branch/worktree remains for coordinator review; Task P is not
merged into devel or pushed. Each cited outer manifest was verified with `sha256sum -c`.
No active source was modified. No failed result was overwritten.
The driver's final outer loopback teardown is cleanup, not an additional
durability gate: its exit receipts are not separately retained. Checked
orderly restart/unmount evidence comes from E1, E2 and D1; final absence
checks establish cleanup only, not an extra durability proof.

Physical-host TCP/RDMA acceptance, four physical members, endurance RSS and
power-loss qualification remain NOTRUN. A later devel/release candidate needs
its own integrated acceptance. User-authorized SIGKILL in the rehearsal is
not power-loss proof. No changes to devops/cluster repositories are needed.
