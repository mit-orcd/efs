# Heartbeat shutdown wakeup — October 28, 2026

Branch `agent/heartbeat-stop-wakeup`, base `69e7c0821`; product/regression
`6dc0bafa8`, tested build and measurement driver `a9bd8600c`.
**Qualified in scope on intel1:** eighteen checked real-daemon stops PASS;
focused normal/TSan/ASan checks PASS; full gate **520 commands, 0 FAIL,
12 NOTRUN / INCOMPLETE, 652 s, 5 OVERBUDGET, 12 s build**. Budgets unchanged.
No merge, push or deployment. Missing hardware/external-tool scopes stay open.

## Problem and change

Wave 23 fixed K51's teardown race by joining the heartbeat before freeing
the Raft host. Its unconditional two-second sleep then delayed that join;
the merge gate recorded eleven over-budget daemon tests. Removing the join
would restore the race. Raising budgets would retain the avoidable wait.

The heartbeat now waits on a private condition variable using
`CLOCK_MONOTONIC`. Shutdown publishes `running=0`, broadcasts while holding
the wait mutex, then joins before destroying the wait objects or the Raft
host. The locked predicate/wait handoff prevents a missed shutdown wakeup.
Spurious wakes retain the original due time. The ordinary two-second
interval, liveness/probe deadlines, durability barriers and shutdown deadline
are unchanged. An already executing probe/status callback still finishes
before the join returns; this change does not cancel network I/O or promise
that every complete daemon stop finishes in a particular subsecond bound.

The synchronization objects are initialized before spawning and cleaned on
spawn/setup failure and after join. Early Raft/packer startup-error exits
also join before tearing down dependencies. The old comment claiming the
heartbeat was detached is corrected (the detached rejoin thread still
requires the process-lifetime server mutex). No product switch, format or
compatibility mechanism is added. Makefile and `tests/budgets.tsv` unchanged.

## Verification

Intel1, private account `maik`, Intel Core Ultra 7 256V, eight CPUs,
15,774,852 KiB RAM: Linux
`6.12.0-211.67.1.el10_2.x86_64`, GCC 14.3.1, Python 3.12.15; host was idle at
preflight (load 0.08/0.11/1.29, no test-account daemon/build workload).
Only the changed source excerpts and harness were staged under
`/home/maik/efs-hb-wakeup-20261028/preflight`. No cluster, transport,
fragment I/O, quota or payload fixture is involved in these pthread checks.
No deployed store, mount, reserved port or other user's process was touched.

`tests/test_heartbeat_shutdown.py` compiles the actual wait/start/stop
functions extracted from `cluster.c`. Its callback is a controlled worker;
it is **not a complete efsd binary or hardware RDMA test**. The source hashes
of all four changed production/test files match `6dc0bafa8` and are recorded
in `SOURCE.sha256`. Build identity: the harness is compiled from those exact
excerpts with GCC, `-std=gnu11 -Wall -Wextra -Werror -pthread` plus the mode's
sanitizer flags. The later complete-daemon build is identified below.

```sh
python3 tests/test_heartbeat_shutdown.py
EFS_TEST_TSAN=1 python3 tests/test_heartbeat_shutdown.py
EFS_TEST_SANITIZE=1 python3 tests/test_heartbeat_shutdown.py
```

All three PASS. The test uses a ten-second interval for prompt-stop checks:
twenty stop/restart rounds complete each join in less than 0.8 s, including
stop racing entry into the wait. Two controlled held-worker cases require
stop to keep waiting until the owned callback/pre-wait work is released.
Failed spawn and repeated stop are safe. A separate 80 ms periodic check
injects eight spurious notifications, checks that the deadline passed to
every timed wait is identical, and refuses an early cycle. This is focused
behavioral/race evidence, not an assertion that all eleven gate overruns
have disappeared.

**Negative control:** in a separate copy of `cluster.c`, insert
`usleep(EFS_HEARTBEAT_MS * 1000);` at the start of `heartbeat_wait`, before
`struct timespec due;`, leaving everything else intact. Run the same test:

```sh
EFS_HEARTBEAT_SOURCE=/absolute/path/to/sleep-negative.c \
    python3 tests/test_heartbeat_shutdown.py
```

It fails at the prompt-stop assertion after the ten-second sleep, rc 1;
this deliberate counterexample is preserved in `unit-negative.log`. It is
not counted as an unclassified product failure or silently retried.

## Linux comparison and full-gate qualification

The committed driver `scripts/measure_heartbeat_stop.py` runs nine checked
SIGTERM stops per source (three repetitions at each of 0.05/0.3/0.8 s after
startup), with a fresh single **unformed** seed, TCP, O_DIRECT, 1 GiB quota,
auto-reserved private loopback endpoints and discovery off. It requires
process rc 0 and an orderly exit record with all three barriers `done`, and
keeps closed logs/results. This isolates idle startup/shutdown; it does not
qualify three-member recovery or in-flight payload durability.

The user explicitly approved the source-only upload. Build clean baseline `69e7c0821` and
candidate `a9bd8600c` trees on the same idle Linux host with exact
`EFS_GIT_ID`, `EFS_GIT_BRANCH` and `EFS_GIT_DESCRIBE`. For each:

```sh
make -j8 all
python3 /path/to/candidate/scripts/measure_heartbeat_stop.py \
    --source /path/to/source --work /private/new-work-directory \
    --output /stable/evidence/new-result-directory
# Candidate full code gate, on the isolated source tree:
EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 EFS_FUSE_PREFIX=/usr \
    bash scripts/make_test_summary.sh
```

Keep full verdicts/NOTRUN/overruns and compare per-command timings with the
retained wave-23 gate (`intel1:~/efs-artifacts/merge-wave23-20261028/`). Do not
inflate any budget or retry a failure into PASS. Preserve failed fixtures;
verify closed logs/hashes before pruning successful synthetic stores and
build trees using `~/bin/edelete --force --delete` on the explicitly owned
scratch paths. No test mount is needed for the timing driver; full-gate
mount cleanup must be checked separately.

**Execution:** the user explicitly approved the source-only upload after
its initial automatic-review refusal. Clean baseline/candidate archives were
transferred, hashed, extracted separately and built on the same quiet host.
`screen` refused the system `/run/screen` mode; detached `nohup` was used
without changing host settings. The controller and exact archive/source
manifests are retained with the results.

| Checked idle-daemon stops | Runs | Minimum | Median | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Baseline `69e7c0821` | 9 | 1.218 s | 1.718 s | 1.972 s |
| Candidate `a9bd8600c` | 9 | 0.114 s | 0.164 s | 0.315 s |

Every stop exited rc 0 with `outcome=orderly` and PUT-sync, reclaim-journal
and usage-flush barriers all `done`. These are complete idle-daemon stop
times on an unformed single-seed fixture, not a latency guarantee for an
in-flight probe, a dirty store or a formed cluster. The full suite below
exercises broader daemon lifecycle cases.

Binary identities: baseline `efsd 69e7c0821` built
`2026-10-28T03:38:07Z`; candidate `efsd a9bd8600c` built
`2026-10-28T03:38:45Z`, both branch `agent/heartbeat-stop-wakeup` on intel1.
Baseline/candidate and final full-gate binary SHA-256 lists are retained.
All archived source files passed content-hash verification after their
measurements, including the candidate after the full gate.

### Full gate and timing comparison

Fresh candidate `make test-all`, eight jobs, system FUSE 3.16.2, private
TMPDIR and loopback fault fixtures on O_DIRECT-capable XFS:

```text
TOTAL 520 FAIL 0 NOTRUN 12 STATUS INCOMPLETE (652s) OVERBUDGET 5 BUILD 12s
```

The retained wave-23 intel1 gate at `6967bfda2` reported 794 s, 11 budget
overruns, 0 FAIL and the **same twelve NOTRUN commands and reasons**. The
new wall is 142 s lower. This is one new full run compared with a dated
same-host run, not a repeated/interleaved whole-suite A/B or a promise of
18% speedup on every host. The controlled nine-per-version stop comparison
above isolates the sleep cost more directly.

| Previously over budget | Wave 23 s | Candidate s | Unchanged budget s |
| --- | ---: | ---: | ---: |
| formation | 19.4 | 9.2 | 15 |
| K33 autoid | 55.3 | 28.4 | 40 |
| K33 client view | 25.9 | 19.1 | 25 |
| K33 interrupted join | 15.0 | 6.1 | 10 |
| K33 payload | 47.3 | 31.5 | 45 |
| K38 shared-chunk pressure | 41.4 | 41.4 | 35 |
| stop progress | 65.4 | 57.3 | 50 |
| operator workflows | 19.8 | 13.2 | 15 |
| member limit | 34.7 | 12.6 | 20 |
| B6 refuse | 48.5 | 22.5 | 45 |
| K22 leader admission | 44.3 | 37.7 | 30 |

Eight of the original eleven now meet budget. The remaining five measured
overruns are K35 degraded fsync **80.8/80 s**, K38 **41.4/35 s**, stop
progress **57.3/50 s**, query-delta with `EFS_QUERY_DELTA_DIRS=1`
**12.1/10 s**, and K22 leader admission **37.7/30 s**. All passed their
assertions; no timing row, workload, pool slot or timeout was raised.
Those measurements stay visible rather than being absorbed into a refit.
The 652 s code-test wall meets D44's unchanged 1,100 s target.

The twelve NOTRUN entries are: external cluster stop/remount scripts absent;
`valgrind/memcheck.h` absent; declaration-gate RDMA subcase, native RDMA
transport and seven segmented modes lack usable IB; follower-bound's
stalled-compactor subcase needs the fault build. Other subcases ran. This
is a TCP-host qualification, not new RDMA-hardware, cross-host, power-loss
or complete sanitizer-suite acceptance. Focused TSan/ASan results above
remain distinct from the normal full gate. No application failure was
retried away and no product failure occurred in this qualification.

## Retained evidence and cleanup

`intel1:/home/maik/efs-artifacts/heartbeat-wakeup-20261028/` retains normal,
TSan and ASan logs, the deliberate negative control, both checked-stop
series and exit records, the complete full-gate summary/timings/build output,
the prior wave-23 comparison text, controller, source/archive and binary
hashes, source verification and quiescence records. No raw output is in Git.

The original eight-file focused manifest is preserved unchanged as
`focused-SHA256SUMS`, SHA-256
`97f5b8f84196deb761150861b36bcba0b29f148f76a1a5fe41a663b81f9bd962`.
All **83** pre-cleanup evidence files were independently verified from the
correct directory (`BEFORE-PRUNE.sha256`). An earlier check used the wrong
working directory and reported missing relative paths; the files existed,
and cleanup was blocked until the successful corrected verification. No
evidence was lost.

Fresh checks found zero owned daemons or mounts. After successful source,
result and hash checks, `~/bin/edelete --force --delete` removed only
`/home/maik/efs-hb-wakeup-20261028` (821 MiB of source archives, builds,
preflight source and passing synthetic stores). There were no unresolved
failed workload stores. `PRUNED.txt` and `prune.log` record cleanup; the
intentional negative-control logs remain. The final **86-file** manifest
verified again after cleanup, SHA-256:
`904958a3f589643d986ed00bf6d540629036d5159eedd6bf95bc01637df5a5e2`.

Local architecture docs gate **5/5 PASS**, environment inventory PASS,
whitespace check PASS. The implementation/driver commits are ready for
integration; a host-limited INCOMPLETE is not promoted to global release PASS.
